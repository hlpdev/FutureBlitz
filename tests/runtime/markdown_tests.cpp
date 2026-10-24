#include "fb/markdown.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <entity.h>
}

namespace {

void append_utf8(std::string& out, const unsigned code_point) {
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

/// One of CommonMark's spec examples.
struct example_t {
    int number = 0;
    std::string section;
    std::string markdown;
    std::string html;
};

/// Reads spec.json: an array of objects whose values are strings and numbers, as CommonMark's
/// spec_tests.py writes it.
class spec_reader_t {
public:
    explicit spec_reader_t(std::string text) : m_text(std::move(text)) {}

    std::vector<example_t> examples() {
        std::vector<example_t> examples;
        expect('[');
        while (skip_space() != ']') {
            examples.push_back(example());
            if (skip_space() == ',') {
                ++m_at;
            }
        }

        return examples;
    }

private:
    example_t example() {
        example_t example;
        expect('{');
        while (skip_space() != '}') {
            const std::string key = string();
            expect(':');
            if (skip_space() == '"') {
                const std::string value = string();
                if (key == "markdown") {
                    example.markdown = value;
                } else if (key == "html") {
                    example.html = value;
                } else if (key == "section") {
                    example.section = value;
                }
            } else {
                const int value = number();
                if (key == "example") {
                    example.number = value;
                }
            }

            if (skip_space() == ',') {
                ++m_at;
            }
        }

        ++m_at;
        return example;
    }

    char skip_space() {
        while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\n' ||
                                        m_text[m_at] == '\r' || m_text[m_at] == '\t')) {
            ++m_at;
        }

        return m_at < m_text.size() ? m_text[m_at] : '\0';
    }

    void expect(const char c) {
        REQUIRE(skip_space() == c);
        ++m_at;
    }

    int number() {
        int value = 0;
        while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9') {
            value = (value * 10) + (m_text[m_at] - '0');
            ++m_at;
        }

        return value;
    }

    unsigned hex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value * 16) + digit_value(m_text[m_at++]);
        }

        return value;
    }

    std::string string() {
        expect('"');
        std::string out;
        while (m_text[m_at] != '"') {
            const char c = m_text[m_at++];
            if (c != '\\') {
                out += c;
                continue;
            }

            const char escaped = m_text[m_at++];
            switch (escaped) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                unsigned code_point = hex4();
                // A surrogate pair is one code point.
                if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                    m_at += 2;
                    code_point = 0x10000 + ((code_point - 0xD800) << 10U) + (hex4() - 0xDC00);
                }

                append_utf8(out, code_point);
                break;
            }
            default:
                out += escaped;
                break;
            }
        }

        ++m_at;
        return out;
    }

    std::string m_text;
    std::size_t m_at = 0;
};

/// HTML as CommonMark's normalize.py has it, which its spec_tests.py compares both sides by:
/// whitespace outside a <pre> as one space, none next to a block-level tag, tags and attribute
/// names lowercase and attributes sorted, a self-closing tag as a start tag, and references as
/// what they stand for, but <, >, &, and ", which stay escaped. It's stricter than Python's
/// HTMLParser in places, never laxer: U+00A0 isn't whitespace, an attribute's named references
/// stay, and a <style>'s text is text, so it gives the Python one's result for 1280 of the 1304
/// HTMLs the examples have, and tells apart all that one does. It's a port of normalize.py,
/// whose notices, its own and CommonMark's test software's, are in data/commonmark/NOTICE.md.
class html_normalizer_t {
public:
    std::string normalize(const std::string_view html) {
        std::size_t at = 0;
        while (at < html.size()) {
            if (html.substr(at).starts_with("<![CDATA[")) {
                const std::size_t end = html.find("]]>", at);
                const std::size_t stop = end == std::string_view::npos ? html.size() : end + 3;
                m_output += html.substr(at, stop - at);
                at = stop;
            } else if (html[at] == '<') {
                at = markup(html, at);
            } else {
                const std::size_t end = html.find('<', at);
                data(html.substr(at, end == std::string_view::npos ? end : end - at));
                at = end == std::string_view::npos ? html.size() : end;
            }
        }

        return m_output;
    }

private:
    enum class last_t : std::uint8_t {
        START_TAG,
        END_TAG,
        OTHER,
    };

    static bool is_block_tag(const std::string_view tag) {
        static constexpr std::array<std::string_view, 50> blocks{
            "article",  "header", "aside",    "hgroup",   "blockquote", "hr",         "iframe",
            "body",     "li",     "map",      "button",   "object",     "canvas",     "ol",
            "caption",  "output", "col",      "p",        "colgroup",   "pre",        "dd",
            "progress", "div",    "section",  "dl",       "table",      "td",         "dt",
            "tbody",    "embed",  "textarea", "fieldset", "tfoot",      "figcaption", "th",
            "figure",   "thead",  "footer",   "tr",       "form",       "ul",         "h1",
            "h2",       "h3",     "h4",       "h5",       "h6",         "video",      "script",
            "style",
        };
        return std::ranges::contains(blocks, tag);
    }

    static bool is_space(const char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    static std::string_view strip_left(std::string_view text) {
        while (!text.empty() && is_space(text.front())) {
            text.remove_prefix(1);
        }

        return text;
    }

    void strip_output() {
        while (!m_output.empty() && is_space(m_output.back())) {
            m_output.pop_back();
        }
    }

    /// Text between tags, with its references as what they stand for.
    void data(std::string_view text) {
        while (!text.empty()) {
            const std::size_t amp = text.find('&');
            const std::string_view plain = text.substr(0, amp);
            text.remove_prefix(plain.size());
            if (!plain.empty()) {
                plain_data(plain);
            }

            if (text.empty()) {
                break;
            }

            const std::size_t semicolon = text.find(';');
            const std::string_view reference =
                semicolon == std::string_view::npos ? text : text.substr(0, semicolon + 1);
            text.remove_prefix(reference.size());
            m_output += referenced(reference);
            m_last = last_t::OTHER;
        }
    }

    void plain_data(std::string_view text) {
        const bool after_tag = m_last == last_t::START_TAG || m_last == last_t::END_TAG;
        if (after_tag && m_last_tag == "br") {
            while (!text.empty() && text.front() == '\n') {
                text.remove_prefix(1);
            }
        }

        std::string collapsed;
        if (m_in_pre) {
            collapsed = text;
        } else {
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (!is_space(text[i])) {
                    collapsed += text[i];
                } else if (i == 0 || !is_space(text[i - 1])) {
                    collapsed += ' ';
                }
            }
        }

        std::string_view result = collapsed;
        if (after_tag && is_block_tag(m_last_tag) && !m_in_pre) {
            result = strip_left(result);
            if (m_last == last_t::END_TAG) {
                while (!result.empty() && is_space(result.back())) {
                    result.remove_suffix(1);
                }
            }
        }

        m_output += result;
        m_last = last_t::OTHER;
    }

    /// "&amp;", "&#35;", or "&copy;" as what it stands for, < > & and " escaped again; one that
    /// isn't a reference stays as it is.
    static std::string referenced(const std::string_view reference) {
        if (reference.size() < 3 || reference.back() != ';') {
            return std::string(reference);
        }

        const std::string_view name = reference.substr(1, reference.size() - 2);
        std::string character;
        // A numeric one has digits of its base, as Python's HTMLParser takes it, and one past
        // U+10FFFF, which Python's chr() refuses, stays as it is.
        if (name.starts_with('#')) {
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            const unsigned base = hex ? 16 : 10;
            const std::string_view digits = name.substr(hex ? 2 : 1);
            const auto is_digit = [hex](const char c) {
                return (c >= '0' && c <= '9') ||
                       (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
            };
            if (digits.empty() || !std::ranges::all_of(digits, is_digit)) {
                return std::string(reference);
            }

            unsigned code_point = 0;
            for (const char digit : digits) {
                code_point = (code_point * base) + digit_value(digit);
                if (code_point > 0x10FFFF) {
                    return std::string(reference);
                }
            }

            append_utf8(character, code_point);
        } else if (const ENTITY* found = entity_lookup(reference.data(), reference.size())) {
            append_utf8(character, found->codepoints[0]);
            if (found->codepoints[1] != 0) {
                append_utf8(character, found->codepoints[1]);
            }
        } else {
            return std::string(reference);
        }

        if (character == "<") {
            return "&lt;";
        }

        if (character == ">") {
            return "&gt;";
        }

        if (character == "&") {
            return "&amp;";
        }

        if (character == "\"") {
            return "&quot;";
        }

        return character;
    }

    /// A tag, comment, declaration, or processing instruction from `at`, which gives where it
    /// ends.
    std::size_t markup(const std::string_view html, const std::size_t at) {
        if (html.substr(at).starts_with("<!--")) {
            const std::size_t end = html.find("-->", at + 4);
            const std::size_t stop = end == std::string_view::npos ? html.size() : end + 3;
            m_output += html.substr(at, stop - at);
            m_last = last_t::OTHER;
            return stop;
        }

        const std::size_t end = html.find('>', at);
        const std::size_t stop = end == std::string_view::npos ? html.size() : end + 1;
        const std::string_view tag = html.substr(at, stop - at);
        if (tag.size() > 1 && (tag[1] == '!' || tag[1] == '?')) {
            m_output += tag;
            m_last = last_t::OTHER;
        } else if (tag.size() > 2 && tag[1] == '/') {
            end_tag(lowered(trimmed_name(tag.substr(2))));
        } else {
            start_tag(tag);
        }

        return stop;
    }

    static std::string lowered(const std::string_view text) {
        std::string out(text);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }

        return out;
    }

    static std::string_view trimmed_name(const std::string_view text) {
        std::size_t length = 0;
        while (length < text.size() && !is_space(text[length]) && text[length] != '>' &&
               text[length] != '/') {
            ++length;
        }

        return text.substr(0, length);
    }

    void end_tag(const std::string& name) {
        if (name == "pre") {
            m_in_pre = false;
        } else if (is_block_tag(name)) {
            strip_output();
        }

        m_output += "</" + name + ">";
        m_last_tag = name;
        m_last = last_t::END_TAG;
    }

    /// Whether a tag's text, between its '<' and '>', ends with the '/' of a self-closing tag,
    /// rather than an unquoted value's last character, as in <a href=x/>, as Python's
    /// HTMLParser reads it: after a space or a quote, or a name's.
    static bool self_closes(const std::string_view rest) {
        if (rest.empty() || rest.back() != '/') {
            return false;
        }

        const std::string_view before = rest.substr(0, rest.size() - 1);
        if (before.empty() || is_space(before.back()) || before.back() == '"' ||
            before.back() == '\'') {
            return true;
        }

        std::size_t start = before.size();
        while (start > 0 && !is_space(before[start - 1])) {
            --start;
        }

        return before.substr(start).find('=') == std::string_view::npos;
    }

    /// "<a href="x" title='y'>" or "<br />": its name and sorted attributes, values decoded and
    /// escaped again.
    void start_tag(const std::string_view tag) {
        std::string_view rest = tag.substr(1, tag.size() - 2);
        const bool closes = self_closes(rest);
        if (closes) {
            rest.remove_suffix(1);
        }

        const std::string name = lowered(trimmed_name(rest));
        rest.remove_prefix(std::min(rest.size(), trimmed_name(rest).size()));
        std::vector<std::pair<std::string, std::optional<std::string>>> attributes;
        for (rest = strip_left(rest); !rest.empty(); rest = strip_left(rest)) {
            std::size_t length = 0;
            while (length < rest.size() && !is_space(rest[length]) && rest[length] != '=') {
                ++length;
            }

            std::string key = lowered(rest.substr(0, length));
            rest.remove_prefix(length);
            rest = strip_left(rest);
            std::optional<std::string> value;
            if (!rest.empty() && rest.front() == '=') {
                rest = strip_left(rest.substr(1));
                std::string_view raw;
                if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
                    const std::size_t close = rest.find(rest.front(), 1);
                    raw = rest.substr(1, close == std::string_view::npos ? close : close - 1);
                    rest.remove_prefix(close == std::string_view::npos ? rest.size() : close + 1);
                } else {
                    std::size_t end = 0;
                    while (end < rest.size() && !is_space(rest[end])) {
                        ++end;
                    }

                    raw = rest.substr(0, end);
                    rest.remove_prefix(end);
                }

                value = escaped(unescaped(raw));
            }

            attributes.emplace_back(std::move(key), std::move(value));
        }

        std::ranges::sort(attributes);
        if (name == "pre") {
            m_in_pre = true;
        }

        if (is_block_tag(name)) {
            strip_output();
        }

        m_output += "<" + name;
        for (const auto& [key, value] : attributes) {
            m_output += " " + key;
            if (value) {
                m_output += "=\"" + *value + "\"";
            }
        }

        m_output += '>';
        m_last_tag = name;
        m_last = closes ? last_t::END_TAG : last_t::START_TAG;
    }

    /// What &lt;, &gt;, &amp;, or &quot; stands for, by its name, or '\0' for another name.
    static char escape_of(const std::string_view name) {
        if (name == "lt") {
            return '<';
        }

        if (name == "gt") {
            return '>';
        }

        if (name == "amp") {
            return '&';
        }

        return name == "quot" ? '"' : '\0';
    }

    /// An attribute's value with its numeric references and &lt;, &gt;, &amp;, and &quot; as
    /// what they stand for.
    static std::string unescaped(const std::string_view raw) {
        std::string out;
        std::size_t at = 0;
        while (at < raw.size()) {
            const std::size_t semicolon = raw.find(';', at);
            if (raw[at] == '&' && semicolon != std::string_view::npos) {
                const std::string_view name = raw.substr(at + 1, semicolon - at - 1);
                if (const char escape = escape_of(name); escape != '\0') {
                    out += escape;
                    at = semicolon + 1;
                    continue;
                }

                if (name.starts_with('#')) {
                    const std::string character = referenced(raw.substr(at, semicolon - at + 1));
                    out += unescaped(character);
                    at = semicolon + 1;
                    continue;
                }
            }

            out += raw[at++];
        }

        return out;
    }

    /// Python's html.escape with quotes: &, <, >, ", and '.
    static std::string escaped(const std::string_view text) {
        std::string out;
        for (const char c : text) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#x27;";
                break;
            default:
                out += c;
                break;
            }
        }

        return out;
    }

    std::string m_output;
    last_t m_last = last_t::START_TAG;
    std::string m_last_tag;
    bool m_in_pre = false;
};

std::string normalized(const std::string_view html) {
    return html_normalizer_t().normalize(html);
}

std::vector<example_t> spec_examples() {
    std::ifstream file(FB_TEST_COMMONMARK_SPEC, std::ios::binary);
    REQUIRE(file);
    std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    return spec_reader_t(std::move(text)).examples();
}

/// The text of a String the runtime made, which is released.
std::string take(fb_string_t* string) {
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

std::string html_of(const std::string_view markdown, const std::int32_t extensions = 0) {
    fb_string_t* text =
        fb_string_from_bytes(markdown.data(), static_cast<std::int64_t>(markdown.size()));
    std::string html = take(fb_markdown_html(text, extensions));
    fb_string_release(text);
    return html;
}

/// MarkdownKind's values that the tree's tests look for.
constexpr std::int32_t heading_kind = 6;
constexpr std::int32_t code_block_kind = 7;
constexpr std::int32_t paragraph_kind = 9;
constexpr std::int32_t link_kind = 18;
constexpr std::int32_t image_kind = 19;
constexpr std::int32_t code_kind = 20;
constexpr std::int32_t text_kind = 22;
constexpr std::int32_t soft_break_kind = 23;
constexpr std::int32_t html_kind = 25;

/// One of fb_markdown_parse()'s events, as its fb_markdown_ functions give it.
struct event_t {
    std::int32_t step = 0;
    std::int32_t kind = 0;
    std::int64_t number = 0;
    std::int32_t flag = 0;
    std::array<std::string, 5> texts;
};

std::vector<event_t> events_of(const std::string_view markdown, const std::int32_t extensions = 0) {
    fb_string_t* text =
        fb_string_from_bytes(markdown.data(), static_cast<std::int64_t>(markdown.size()));
    void* events = fb_markdown_parse(text, extensions);
    fb_string_release(text);
    std::vector<event_t> out;
    for (std::int64_t i = 0; i < fb_markdown_count(events); ++i) {
        event_t event{
            .step = fb_markdown_step(events, i),
            .kind = fb_markdown_kind(events, i),
            .number = fb_markdown_number(events, i),
            .flag = fb_markdown_flag(events, i),
        };
        for (std::size_t which = 0; which < event.texts.size(); ++which) {
            event.texts.at(which) =
                take(fb_markdown_text(events, i, static_cast<std::int32_t>(which)));
        }

        out.push_back(std::move(event));
    }

    fb_markdown_free(events);
    return out;
}

/// The events of the kind, in order.
std::vector<event_t> of_kind(const std::vector<event_t>& events, const std::int32_t kind) {
    std::vector<event_t> out;
    for (const event_t& event : events) {
        // A block's or span's leaving has nothing of its own.
        if (event.kind == kind && event.step != 1) {
            out.push_back(event);
        }
    }

    return out;
}

} // namespace

TEST_CASE("Markdown passes every one of CommonMark 0.31.2's spec examples", "[runtime]") {
    const std::vector<example_t> examples = spec_examples();
    REQUIRE(examples.size() == 652);
    // As CommonMark's spec_tests.py compares them, which a <br> and a <br /> are alike to, as
    // a newline after a <li> is.
    std::size_t passed = 0;
    for (const example_t& example : examples) {
        const std::string html = normalized(html_of(example.markdown));
        const std::string expected = normalized(example.html);
        if (html == expected) {
            ++passed;
            continue;
        }

        INFO("example " << example.number << " (" << example.section << ")\n" << example.markdown);
        CHECK(html == expected);
    }

    CHECK(passed == examples.size());
}

TEST_CASE("a tree's links, images, and code blocks have their entities decoded", "[runtime]") {
    const std::vector<event_t> events =
        events_of("[a](/u&amp;v \"t&#33; &bogus;\") ![b](c&#x2F;d.png) "
                  "<https://example.com>\n\n"
                  "```c&amp;d e\nx\n```\n");
    const std::vector<event_t> links = of_kind(events, link_kind);
    REQUIRE(links.size() == 2);
    CHECK(links[0].texts[1] == "/u&v");
    CHECK(links[0].texts[2] == "t! &bogus;");
    CHECK(links[0].flag == 0);
    CHECK(links[1].texts[1] == "https://example.com");
    CHECK(links[1].texts[2].empty());
    CHECK(links[1].flag == 1);

    const std::vector<event_t> images = of_kind(events, image_kind);
    REQUIRE(images.size() == 1);
    CHECK(images[0].texts[1] == "c/d.png");

    const std::vector<event_t> blocks = of_kind(events, code_block_kind);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].texts[0] == "x\n");
    CHECK(blocks[0].texts[3] == "c&d e");
    CHECK(blocks[0].texts[4] == "c&d");
}

TEST_CASE("a tree's text is one event for each run of it, with its entities decoded", "[runtime]") {
    // A document, a paragraph, its text, and their leaving.
    const std::vector<event_t> events = events_of("a &amp; b&#x41;&copy;");
    REQUIRE(events.size() == 5);
    CHECK(events[2].step == 2);
    CHECK(events[2].kind == text_kind);
    CHECK(events[2].texts[0] == "a & bA\xC2\xA9");

    // 0, a surrogate, and one past U+10FFFF are U+FFFD, as CommonMark has them, and a named
    // entity can be two code points.
    const std::vector<event_t> numbers = of_kind(
        events_of("&#0; &#xD800; &#1114112; &#x10FFFF; &NotEqualTilde; &bogus;"), text_kind);
    REQUIRE(numbers.size() == 1);
    CHECK(numbers[0].texts[0] ==
          "\xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD \xF4\x8F\xBF\xBF \xE2\x89\x82\xCC\xB8 &bogus;");
}

TEST_CASE("an indented code block has no info string or language", "[runtime]") {
    const std::vector<event_t> blocks =
        of_kind(events_of("    code\n\n```\nfenced\n```\n"), code_block_kind);
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0].texts[0] == "code\n");
    CHECK(blocks[0].texts[3].empty());
    CHECK(blocks[0].texts[4].empty());
    CHECK(blocks[1].texts[0] == "fenced\n");
    CHECK(blocks[1].texts[3].empty());
    CHECK(blocks[1].texts[4].empty());
}

TEST_CASE("a NUL in text, a code span, or a code block is U+FFFD, in HTML and in a tree",
          "[runtime]") {
    using namespace std::string_view_literals;
    // md4c alone keeps a code span's NUL, and puts a code block's after its U+FFFD.
    const std::string_view markdown = "a\0b `\0` `x\0y`\n\n    \0\n    x\0y\n"sv;
    CHECK(html_of(markdown) == "<p>a\xEF\xBF\xBD"
                               "b <code>\xEF\xBF\xBD</code> <code>x\xEF\xBF\xBDy</code></p>\n"
                               "<pre><code>\xEF\xBF\xBD\nx\xEF\xBF\xBDy\n</code></pre>\n");

    const std::vector<event_t> events = events_of(markdown);
    const std::vector<event_t> texts = of_kind(events, text_kind);
    REQUIRE(texts.size() == 2);
    CHECK(texts[0].texts[0] == "a\xEF\xBF\xBD"
                               "b ");
    CHECK(texts[1].texts[0] == " ");

    const std::vector<event_t> spans = of_kind(events, code_kind);
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].texts[0] == "\xEF\xBF\xBD");
    CHECK(spans[1].texts[0] == "x\xEF\xBF\xBDy");

    const std::vector<event_t> blocks = of_kind(events, code_block_kind);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].texts[0] == "\xEF\xBF\xBD\nx\xEF\xBF\xBDy\n");
    CHECK(of_kind(events, paragraph_kind).size() == 1);
}

TEST_CASE("a raw HTML tag or comment that goes on to other lines is one Html", "[runtime]") {
    const std::vector<event_t> tag =
        of_kind(events_of("a <span   \nclass=\"x\">b</span>"), html_kind);
    REQUIRE(tag.size() == 2);
    CHECK(tag[0].texts[0] == "<span   \nclass=\"x\">");
    CHECK(tag[1].texts[0] == "</span>");
    const std::vector<event_t> comment =
        of_kind(events_of("a <!-- one\ntwo\nthree --> b"), html_kind);
    REQUIRE(comment.size() == 1);
    CHECK(comment[0].texts[0] == "<!-- one\ntwo\nthree -->");

    // Tags on lines of their own, or side by side, are each one.
    const std::vector<event_t> apart = events_of("a <i>\n</i><b>");
    REQUIRE(of_kind(apart, html_kind).size() == 3);
    CHECK(of_kind(apart, soft_break_kind).size() == 1);
}

TEST_CASE("a code span in a table's cell takes \\| as a |, in HTML and in a tree", "[runtime]") {
    // GitHub's tables, example 200.
    const std::string_view table = "| f\\|oo  |\n| ------ |\n| b `\\|` az |\n| b **\\|** im |";
    const std::string html = html_of(table, 1);
    CHECK(html.contains("<th>f|oo</th>"));
    CHECK(html.contains("<td>b <code>|</code> az</td>"));
    CHECK(html.contains("<td>b <strong>|</strong> im</td>"));
    const std::vector<event_t> code = of_kind(events_of(table, 1), code_kind);
    REQUIRE(code.size() == 1);
    CHECK(code[0].texts[0] == "|");

    // One outside a table keeps its backslash, as CommonMark has it, and "\\|" is a "\|".
    CHECK(html_of("`a \\| b`") == "<p><code>a \\| b</code></p>\n");
    CHECK(html_of("| a |\n|---|\n| `\\\\|` |", 1).contains("<td><code>\\|</code></td>"));
}

TEST_CASE("a byte order mark at Markdown's start isn't its text", "[runtime]") {
    CHECK(html_of("\xEF\xBB\xBF# Title\n\ntext") == "<h1>Title</h1>\n<p>text</p>\n");
    const std::vector<event_t> headings =
        of_kind(events_of("\xEF\xBB\xBF# Title\n\ntext"), heading_kind);
    REQUIRE(headings.size() == 1);
    CHECK(of_kind(events_of("\xEF\xBB\xBF# Title"), text_kind)[0].texts[0] == "Title");
    // Only there.
    CHECK(html_of("a\xEF\xBB\xBF") == "<p>a\xEF\xBB\xBF</p>\n");
}

TEST_CASE("a numeric reference to a surrogate is U+FFFD in HTML, as in a tree", "[runtime]") {
    CHECK(html_of("&#xD800; [a](/&#55296; \"&#xDFFF;\")") ==
          "<p>\xEF\xBF\xBD <a href=\"/%EF%BF%BD\" title=\"\xEF\xBF\xBD\">a</a></p>\n");
    CHECK(html_of("&#xD7FF; &#xE000;") == "<p>\xED\x9F\xBF \xEE\x80\x80</p>\n");
}

TEST_CASE("Markdown of less than 4 GiB is what md4c reads", "[runtime]") {
    CHECK(fb::markdown_fits(0));
    CHECK(fb::markdown_fits(4294967295));
    CHECK_FALSE(fb::markdown_fits(4294967296));
}

TEST_CASE("the normalizer tells apart what normalize.py does", "[runtime]") {
    // normalize.py keeps "&#;" and "&#x;", and one past U+10FFFF, as they are.
    CHECK(normalized("<p>&#;</p>") == "<p>&#;</p>");
    CHECK(normalized("<p>&#x;</p>") == "<p>&#x;</p>");
    CHECK(normalized("<p>&#1114112;</p>") == "<p>&#1114112;</p>");
    CHECK(normalized("<p>&#12a;</p>") == "<p>&#12a;</p>");
    CHECK(normalized("<p>&#x41;&#65;</p>") == "<p>AA</p>");
    // An unquoted value's '/' is the value's, so only the others close their tags.
    CHECK(normalized("<a href=x/>") == "<a href=\"x/\">");
    CHECK(normalized("<a href=x />") == "<a href=\"x\">");
    CHECK(normalized("<a href=\"x\"/>") == "<a href=\"x\">");
    CHECK(normalized("<br/><input disabled/>") == "<br><input disabled>");
}
