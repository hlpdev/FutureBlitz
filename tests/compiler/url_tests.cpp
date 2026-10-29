#include "fb/driver.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The WHATWG URL Standard's own tests, web-platform-tests' urltestdata.json in tests/data, run
// through std.net's Url: each case becomes a check in one program, which prints each case that
// doesn't pass, by its number in the file.

namespace {

/// A JSON value, of what urltestdata.json has: null, a Bool, a String, an array, or an object.
struct json_t {
    enum class kind_t : std::uint8_t { NUL, BOOL, STRING, ARRAY, OBJECT } kind = kind_t::NUL;
    bool flag = false;
    std::string text;
    std::vector<json_t> items;
    std::vector<std::pair<std::string, json_t>> fields;

    /// The field of `name`, or nullptr when the object has none.
    [[nodiscard]] const json_t* field(const std::string_view name) const {
        for (const auto& [key, value] : fields) {
            if (key == name) {
                return &value;
            }
        }

        return nullptr;
    }
};

/// Reads JSON's values. A lone surrogate, which a JavaScript String can hold, becomes U+FFFD,
/// as the URL constructor makes it.
class json_reader_t {
public:
    explicit json_reader_t(const std::string_view text) : m_text(text) {}

    json_t value() {
        skip_space();
        json_t value;
        const char c = m_text.at(m_at);
        if (c == '{') {
            value.kind = json_t::kind_t::OBJECT;
            ++m_at;
            while (skip_space(), m_text.at(m_at) != '}') {
                std::string key = string();
                skip_space();
                ++m_at; // :
                value.fields.emplace_back(std::move(key), this->value());
                skip_space();
                if (m_text.at(m_at) == ',') {
                    ++m_at;
                }
            }

            ++m_at;
        } else if (c == '[') {
            value.kind = json_t::kind_t::ARRAY;
            ++m_at;
            while (skip_space(), m_text.at(m_at) != ']') {
                value.items.push_back(this->value());
                skip_space();
                if (m_text.at(m_at) == ',') {
                    ++m_at;
                }
            }

            ++m_at;
        } else if (c == '"') {
            value.kind = json_t::kind_t::STRING;
            value.text = string();
        } else if (m_text.substr(m_at, 4) == "true" || m_text.substr(m_at, 5) == "false") {
            value.kind = json_t::kind_t::BOOL;
            value.flag = c == 't';
            m_at += value.flag ? 4 : 5;
        } else {
            m_at += 4; // null
        }

        return value;
    }

private:
    std::string_view m_text;
    std::size_t m_at = 0;

    void skip_space() {
        while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\n' ||
                                        m_text[m_at] == '\r' || m_text[m_at] == '\t')) {
            ++m_at;
        }
    }

    std::uint32_t hex4() {
        const auto value = static_cast<std::uint32_t>(
            std::stoul(std::string(m_text.substr(m_at, 4)), nullptr, 16));
        m_at += 4;
        return value;
    }

    std::string string() {
        std::string text;
        ++m_at; // "
        while (m_text.at(m_at) != '"') {
            const char c = m_text[m_at++];
            if (c != '\\') {
                text += c;
                continue;
            }

            const char escaped = m_text[m_at++];
            switch (escaped) {
            case 'n':
                text += '\n';
                break;
            case 't':
                text += '\t';
                break;
            case 'r':
                text += '\r';
                break;
            case 'b':
                text += '\b';
                break;
            case 'f':
                text += '\f';
                break;
            case 'u': {
                std::uint32_t point = hex4();
                const bool high = point >= 0xD800 && point < 0xDC00;
                if (high && m_text.substr(m_at, 2) == "\\u") {
                    m_at += 2;
                    const std::uint32_t low = hex4();
                    if (low >= 0xDC00 && low < 0xE000) {
                        point = 0x10000 + ((point - 0xD800) << 10U) + (low - 0xDC00);
                    } else {
                        fb::append_utf8(text, 0xFFFD);
                        point = low >= 0xD800 && low < 0xE000 ? 0xFFFD : low;
                    }
                } else if (point >= 0xD800 && point < 0xE000) {
                    point = 0xFFFD;
                }

                fb::append_utf8(text, point);
                break;
            }
            default:
                text += escaped;
                break;
            }
        }

        ++m_at; // "
        return text;
    }
};

/// A String literal of `text` for a FutureBlitz program, with escapes for what isn't printable
/// ASCII, and for its quotes, backslashes, and braces.
std::string literal(const std::string& text) {
    std::string result = "\"";
    for (std::size_t i = 0; i < text.size();) {
        const fb::decoded_t decoded = fb::decode_utf8(text, i);
        const auto point = static_cast<std::uint32_t>(decoded.code_point);
        i += decoded.length;
        if (point == '"' || point == '\\') {
            result += '\\';
            result += static_cast<char>(point);
        } else if (point == '{' || point == '}') {
            result += std::string(2, static_cast<char>(point));
        } else if (point < 0x20 || point >= 0x7F) {
            result += std::format("\\u{{{:X}}}", point);
        } else {
            result += static_cast<char>(point);
        }
    }

    return result + "\"";
}

/// A string field's literal, or Null for a field the case doesn't have, which isn't checked.
std::string field_literal(const json_t& test, const std::string_view name) {
    const json_t* field = test.field(name);
    return field != nullptr && field->kind == json_t::kind_t::STRING ? literal(field->text)
                                                                     : "Null";
}

/// The program's start: its checks of a case that's a URL and of one that isn't.
constexpr std::string_view checks = R"fb(Import std.core
Import std.net

Global names = ["href", "origin", "protocol", "username", "password", "host", "hostname",
                "port", "pathname", "search", "hash", "searchParams"]

// What JavaScript's URL gives for each getter, by its number in names.
Function Getter(url: Url, field: Int): String
    Local host = url.Host ?? ""
    Local port = url.Port
    Local query = url.Query ?? ""
    Local fragment = url.Fragment ?? ""
    Select field
        Case 0
            Return url.ToString()
        Case 1
            Return url.Origin
        Case 2
            Return url.Scheme + ":"
        Case 3
            Return url.Username
        Case 4
            Return url.Password
        Case 5
            Return port == Null ? host : "{host}:{port}"
        Case 6
            Return host
        Case 7
            Return port == Null ? "" : "{port}"
        Case 8
            Return url.Path
        Case 9
            Return query == "" ? "" : "?" + query
        Case 10
            Return fragment == "" ? "" : "#" + fragment
        Default
            Return url.Params.ToString()
    End Select
End Function

// The URL input makes against base, or Null when it isn't one.
Function Parsed(input: String, base: String?): Url?
    If base == Null Then Return Url.Parse(input)
    Return Url.Parse(input, base)
End Function

// A case that's a URL, whose getters give what expected has, but where it's Null.
Function Check(number: Int, input: String, base: String?, expected: String?[])
    Local url = Parsed(input, base)
    If url == Null
        Print("{number} isn't a URL")
        Return
    End If

    For field = 0 Until expected.Count
        Local wanted = expected[field]
        If wanted != Null && Getter(url, field) != wanted
            Print("{number} {names[field]} is {Getter(url, field)}")
        End If
    End For
End Function

// A case that isn't a URL, against its base or without one. Its relativeTo says only that it
// would be one against another base, which web-platform-tests uses to skip it in a page.
Function Fail(number: Int, input: String, base: String?)
    Local url = Parsed(input, base)
    If url != Null Then Print("{number} is {url}")
End Function

)fb";

/// urltestdata.json's cases, by their number among the cases, as checks.
std::string program(const json_t& data) {
    std::string text(checks);
    int number = 0;
    for (const json_t& test : data.items) {
        if (test.kind != json_t::kind_t::OBJECT) {
            continue;
        }

        const std::string input = field_literal(test, "input");
        const std::string base = field_literal(test, "base");
        const json_t* failure = test.field("failure");
        if (failure != nullptr && failure->flag) {
            text += std::format("Fail({}, {}, {})\n", number, input, base);
        } else {
            text += std::format("Check({}, {}, {}, [", number, input, base);
            constexpr std::array<std::string_view, 12> names{
                "href",     "origin", "protocol", "username", "password", "host",
                "hostname", "port",   "pathname", "search",   "hash",     "searchParams",
            };
            for (std::size_t i = 0; i < names.size(); ++i) {
                text += std::format("{}{}", i == 0 ? "" : ", ", field_literal(test, names[i]));
            }

            text += "])\n";
        }

        ++number;
    }

    return text;
}

/// The numbers of the cases a program's output says didn't pass.
std::set<int> failed(const std::string& output) {
    std::set<int> numbers;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        numbers.insert(std::stoi(line));
    }

    return numbers;
}

} // namespace

TEST_CASE("Url passes the WHATWG URL Standard's tests that apply", "[programs]") {
    std::ifstream file(std::filesystem::path(FB_TEST_DATA) / "urltestdata.json", std::ios::binary);
    REQUIRE(file);
    const std::string json{ std::istreambuf_iterator<char>(file), {} };
    const json_t data = json_reader_t(json).value();
    REQUIRE(data.items.size() > 1000);

    const std::vector<fb::library_file_t> library =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation = fb::compile("main.fb", program(data), library);
    if (compilation.diagnostics.has_errors()) {
        const fb::diagnostic_t& first = compilation.diagnostics.diagnostics().front();
        FAIL(std::format("{}: {}", compilation.sources.get(first.span.file).slice(first.span),
                         first.message));
    }

    std::filesystem::path executable = std::filesystem::path(FB_TEST_OUTPUT) / "url_wpt";
    executable += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::expected<fb::process_result_t, std::string> ran =
        fb::build_program(module, compilation, executable, fb::build_mode_t::DEBUG).and_then([&] {
            return fb::run_process(executable, {});
        });
    REQUIRE(ran);
    INFO(ran->output);
    CHECK(ran->exit_code == 0);

    // The cases that need UTS #46's mapping of what isn't ASCII in a host, which waits for
    // std.text.unicode, by their numbers.
    const std::set<int> mapping{ 732 };
    CHECK(failed(ran->output) == mapping);
}
