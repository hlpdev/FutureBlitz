#include "fb/runtime.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// The text of a String the runtime made, which is released.
std::string take(fb_string_t* string) {
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

/// A String the runtime made from `text`, which goes with the text_t.
class text_t {
public:
    explicit text_t(const std::string_view text)
        : m_string(fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()))) {}
    text_t(const text_t&) = delete;
    text_t& operator=(const text_t&) = delete;
    ~text_t() {
        fb_string_release(m_string);
    }

    operator const fb_string_t*() const {
        return m_string;
    }

private:
    fb_string_t* m_string;
};

void free_elements(fb_object_t* object) {
    fb_array_free(reinterpret_cast<fb_array_t*>(object));
}

void release_strings(fb_object_t* object) {
    auto* array = reinterpret_cast<fb_array_t*>(object);
    for (std::int64_t i = 0; i < array->count; ++i) {
        fb_string_release(static_cast<fb_string_t* const*>(array->data)[i]);
    }

    fb_array_free(array);
}

void visit_nothing(const fb_object_t* /*object*/, fb_visitor_t /*visitor*/, void* /*context*/) {}

fb_class_t array_class(void (*release)(fb_object_t*)) {
    return {
        .name = "array",
        .size = sizeof(fb_array_t),
        .destroy = nullptr,
        .release = release,
        .visit = visit_nothing,
        .live = 0,
        .base = nullptr,
    };
}

/// The steps fb_value_path takes `path` apart into, a key or an index each, or nullopt-like
/// empty steps and false for text that isn't a path.
std::pair<bool, std::vector<std::pair<std::string, std::int64_t>>>
steps(const std::string_view path) {
    fb_class_t strings = array_class(release_strings);
    fb_class_t numbers = array_class(free_elements);
    fb_array_t* keys = fb_array_new(&strings, sizeof(fb_string_t*), 0);
    fb_array_t* indices = fb_array_new(&numbers, sizeof(std::int64_t), 0);
    const bool valid = fb_value_path(text_t(path), keys, indices) != 0;
    std::vector<std::pair<std::string, std::int64_t>> found;
    for (std::int64_t i = 0; i < keys->count; ++i) {
        const fb_string_t* key = static_cast<fb_string_t* const*>(keys->data)[i];
        found.emplace_back(std::string(reinterpret_cast<const char*>(key + 1),
                                       static_cast<std::size_t>(key->length)),
                           static_cast<const std::int64_t*>(indices->data)[i]);
    }

    fb_object_release(&keys->header);
    fb_object_release(&indices->header);
    return { valid, found };
}

} // namespace

TEST_CASE("a Value's String is quoted as JSON writes one", "[runtime]") {
    using namespace std::string_view_literals;
    CHECK(take(fb_value_quoted(text_t("plain"))) == "\"plain\"");
    CHECK(take(fb_value_quoted(text_t(""))) == "\"\"");
    CHECK(take(fb_value_quoted(text_t("\" \\ \b \f \n \r \t"))) == R"("\" \\ \b \f \n \r \t")");
    CHECK(take(fb_value_quoted(text_t("\x01\x1f\0"sv))) == R"("\u0001\u001f\u0000")");
    // What isn't a control character stays: a DEL, and UTF-8's characters of several bytes.
    CHECK(take(fb_value_quoted(text_t("\x7f é 😀 /"))) == "\"\x7f é 😀 /\"");
}

TEST_CASE("a Value's bytes are two hex digits each", "[runtime]") {
    fb_class_t numbers = array_class(free_elements);
    fb_array_t* bytes = fb_array_new(&numbers, 1, 0);
    CHECK(take(fb_value_hex(bytes)).empty());
    for (const std::uint8_t byte : std::array<std::uint8_t, 5>{ 0x00, 0x0f, 0x10, 0xab, 0xff }) {
        *static_cast<std::uint8_t*>(fb_array_push(bytes, 1)) = byte;
    }

    CHECK(take(fb_value_hex(bytes)) == "000f10abff");
    fb_object_release(&bytes->header);
}

TEST_CASE("a Value's path is keys after dots and indices in brackets", "[runtime]") {
    using step_t = std::pair<std::string, std::int64_t>;
    using found_t = std::pair<bool, std::vector<step_t>>;
    CHECK(steps("") == found_t{ true, {} });
    CHECK(steps("a") == found_t{ true, { { "a", -1 } } });
    CHECK(steps("server.ports[1].name") ==
          found_t{ true, { { "server", -1 }, { "ports", -1 }, { "", 1 }, { "name", -1 } } });
    CHECK(steps("[0][12]") == found_t{ true, { { "", 0 }, { "", 12 } } });
    CHECK(steps("a b.c-d") == found_t{ true, { { "a b", -1 }, { "c-d", -1 } } });
    CHECK(steps("[9223372036854775807]").first);
    constexpr std::array wrongs{
        "a..b",
        ".a",
        "a.",
        "a[",
        "a[]",
        "a[x]",
        "a[-1]",
        "a[+1]",
        "a]",
        "a[1]b",
        "[99999999999999999999]",
        "a[1",
        "a.[0]",
    };
    for (const std::string_view wrong : wrongs) {
        INFO(wrong);
        CHECK(!steps(wrong).first);
    }
}

TEST_CASE("Load<T> reads a String as a Bool or a number, as a command line's are", "[runtime]") {
    struct read_t {
        std::int32_t kind;
        std::uint64_t bits;
        double real;
    };
    const auto read = [](const std::string_view text) {
        read_t result{ .kind = 0, .bits = 0, .real = 0 };
        result.kind = fb_value_scalar(text_t(text), &result.bits, &result.real);
        return result;
    };
    CHECK(read("true").kind == 1);
    CHECK(read("true").bits == 1);
    CHECK(read("FALSE").kind == 1);
    CHECK(read("False").bits == 0);
    CHECK(read("8080").kind == 2);
    CHECK(read("8080").bits == 8080);
    CHECK(read("-9223372036854775808").bits == 0x8000000000000000U);
    CHECK(read("+12").bits == 12);
    CHECK(read("9223372036854775808").kind == 3);
    CHECK(read("18446744073709551615").bits == UINT64_MAX);
    // An integer past a UInt64's largest, or below an Int64's smallest, is a Double.
    CHECK(read("18446744073709551616").kind == 4);
    CHECK(read("-9223372036854775809").kind == 4);
    CHECK(read("2.5").kind == 4);
    CHECK(read("2.5").real == 2.5);
    CHECK(read("-1e3").real == -1000.0);
    CHECK(read("1e999").real == std::numeric_limits<double>::infinity());
    CHECK(read("-1e999").real == -std::numeric_limits<double>::infinity());
    CHECK(read("1e-999").kind == 4);
    CHECK(read("1e-999").real == 0.0);
    CHECK(read("inf").kind == 4);
    for (const std::string_view wrong : { "", "yes", " 1", "1 ", "1x", "0x10", "++1", "+-1", "-",
                                          "+", "1..2", "true1" }) {
        INFO(wrong);
        CHECK(read(wrong).kind == 0);
    }
}

TEST_CASE("Load<T> reads an Enum's member by its name, in any case, and flags joined by |",
          "[runtime]") {
    fb_class_t strings = array_class(release_strings);
    fb_class_t numbers = array_class(free_elements);
    fb_array_t* names = fb_array_new(&strings, sizeof(fb_string_t*), 0);
    fb_array_t* values = fb_array_new(&numbers, sizeof(std::uint64_t), 0);
    const std::array<std::pair<std::string_view, std::uint64_t>, 4> members{ {
        { "WORLD", 1 },
        { "PLAYER", 2 },
        { "Player", 8 },
        { "ENEMY", 4 },
    } };
    for (const auto& [name, bits] : members) {
        *static_cast<fb_string_t**>(fb_array_push(names, sizeof(fb_string_t*))) =
            fb_string_from_bytes(name.data(), static_cast<std::int64_t>(name.size()));
        *static_cast<std::uint64_t*>(fb_array_push(values, sizeof(std::uint64_t))) = bits;
    }

    const auto read = [&](const std::string_view text, const bool flags) -> std::int64_t {
        std::uint64_t bits = 0;
        if (fb_value_enum(text_t(text), names, values, flags ? 1 : 0, &bits) == 0) {
            return -1;
        }

        return static_cast<std::int64_t>(bits);
    };
    CHECK(read("WORLD", false) == 1);
    CHECK(read("enemy", false) == 4);
    // A name as it's written comes first, then one in another case.
    CHECK(read("Player", false) == 8);
    CHECK(read("player", false) == 2);
    CHECK(read(" WORLD ", false) == 1);
    CHECK(read("WORLD | ENEMY", false) == -1);
    CHECK(read("WORLD | ENEMY", true) == 5);
    CHECK(read("world|player", true) == 3);
    CHECK(read("ENEMY", true) == 4);
    for (const std::string_view wrong : { "", "SKY", "WORLD |", "| WORLD", "WORLD || ENEMY",
                                          "WORLD, ENEMY" }) {
        INFO(wrong);
        CHECK(read(wrong, true) == -1);
        CHECK(read(wrong, false) == -1);
    }

    fb_object_release(&names->header);
    fb_object_release(&values->header);
}

TEST_CASE("Save writes a Float32 as the Double its shortest text reads as", "[runtime]") {
    CHECK(fb_value_float(0.1F) == 0.1);
    CHECK(fb_value_float(1.5F) == 1.5);
    CHECK(std::signbit(fb_value_float(-0.0F)));
    CHECK(fb_value_float(3.4028235e38F) == 3.4028235e38);
    CHECK(fb_value_float(1e-45F) == 1e-45);
    CHECK(fb_value_float(std::numeric_limits<float>::infinity()) ==
          std::numeric_limits<double>::infinity());
    CHECK(std::isnan(fb_value_float(std::numeric_limits<float>::quiet_NaN())));
}
