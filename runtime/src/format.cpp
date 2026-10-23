#include "fb/runtime.hpp"
#include "fb/text_format.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fast_float/fast_float.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Text by a format spec, which the compiler checked, or which a Formattable's Format reads at run
// time: through the formatter the compiler folds constants with, so both give the same text.

namespace {

std::string_view view(const fb_string_t* string) {
    return { reinterpret_cast<const char*>(string + 1), static_cast<std::size_t>(string->length) };
}

fb_string_t* make(const std::string& text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

/// The spec that `text` is, when it fits what it formats.
std::optional<fb::format_spec_t> spec_for(const fb_string_t* text,
                                          const fb::format_subject_t subject) {
    const std::expected<fb::format_spec_t, fb::format_spec_error_t> spec =
        fb::parse_format_spec(view(text));
    if (!spec || fb::format_spec_misfit(*spec, subject, "")) {
        return std::nullopt;
    }

    return *spec;
}

/// What text in `base` can start with: "0x", "0o", "0b", or nothing.
std::string_view prefix_of(const std::int32_t base) {
    switch (base) {
    case 16:
        return "0x";
    case 8:
        return "0o";
    case 2:
        return "0b";
    default:
        return "";
    }
}

/// The digits of an integer in `base`, after an optional sign, which says whether it's
/// `negative`, and the base's "0x", "0o", or "0b", if it has one. nullopt for text that isn't
/// one, or doesn't fit in 64 bits.
std::optional<std::uint64_t> magnitude_of(std::string_view digits, const std::int32_t base,
                                          bool& negative) {
    if (!digits.empty() && (digits.front() == '+' || digits.front() == '-')) {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }

    const std::string_view prefix = prefix_of(base);
    if (!prefix.empty() && digits.size() > 2 && digits.starts_with(prefix)) {
        digits.remove_prefix(2);
    }

    std::uint64_t magnitude = 0;
    const char* first = digits.data();
    const char* end = first + digits.size();
    const std::from_chars_result read = std::from_chars(first, end, magnitude, base);
    if (digits.empty() || read.ec != std::errc{} || read.ptr != end) {
        return std::nullopt;
    }

    return magnitude;
}

/// Adds a new String of `text` to an array of Strings, which takes its reference.
void add_text(fb_array_t* array, const std::string& text) {
    *static_cast<fb_string_t**>(fb_array_push(array, sizeof(fb_string_t*))) = make(text);
}

/// A spec without its kind, which the debug form of a number or a String only pads by.
fb::format_spec_t padding_of(fb::format_spec_t spec) {
    spec.kind = 0;
    spec.alternate = false;
    return spec;
}

} // namespace

extern "C" {

fb_string_t* fb_format_int(const std::int64_t value, const fb_string_t* spec) {
    const std::optional<fb::format_spec_t> read = spec_for(spec, fb::format_subject_t::INTEGER);
    if (!read) {
        return nullptr;
    }

    return make(fb::format_signed(value, read->kind == '?' ? padding_of(*read) : *read));
}

fb_string_t* fb_format_uint(const std::uint64_t value, const fb_string_t* spec) {
    const std::optional<fb::format_spec_t> read = spec_for(spec, fb::format_subject_t::INTEGER);
    if (!read) {
        return nullptr;
    }

    return make(fb::format_integer(value, false, read->kind == '?' ? padding_of(*read) : *read));
}

fb_string_t* fb_format_float(const double value, const std::int32_t float32,
                             const fb_string_t* spec) {
    const std::optional<fb::format_spec_t> read = spec_for(spec, fb::format_subject_t::FLOAT);
    if (!read) {
        return nullptr;
    }

    return make(
        fb::format_float(value, float32 != 0, read->kind == '?' ? padding_of(*read) : *read));
}

fb_string_t* fb_format_text(const fb_string_t* text, const fb_string_t* spec,
                            const std::int32_t string) {
    const std::optional<fb::format_spec_t> read =
        spec_for(spec, string != 0 ? fb::format_subject_t::STRING : fb::format_subject_t::TEXT);
    if (!read) {
        return nullptr;
    }

    // A String's debug form is in quotes, and other text's is the text.
    if (read->kind == '?' && string != 0) {
        return make(fb::format_text(fb::debug_text(view(text)), padding_of(*read)));
    }

    return make(fb::format_text(view(text), padding_of(*read)));
}

fb_string_t* fb_string_debug(const fb_string_t* text) {
    return make(fb::debug_text(view(text)));
}

fb_string_t* fb_debug_object(const fb_object_t* object, const std::int32_t indent) {
    const fb_class_t* type = object->type;
    if (type->debug == nullptr) {
        return make(type->name);
    }

    // The objects this thread is showing, which a cycle comes back to.
    thread_local std::vector<const fb_object_t*> showing;
    if (std::ranges::contains(showing, object)) {
        return make(std::string(type->name) + " { … }");
    }

    showing.push_back(object);
    fb_string_t* text = type->debug(object, indent);
    showing.pop_back();
    return text;
}

fb_string_t* fb_debug_indent(const std::int32_t indent) {
    return make("\n" + std::string(static_cast<std::size_t>(std::max(indent, 0)) * 4, ' '));
}

std::int32_t fb_parse_int(const fb_string_t* text, const std::int32_t base, std::int64_t* value) {
    bool negative = false;
    const std::optional<std::uint64_t> magnitude = magnitude_of(view(text), base, negative);
    const std::uint64_t most =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1 : 0);
    if (!magnitude || *magnitude > most) {
        return 0;
    }

    *value = negative ? static_cast<std::int64_t>(~*magnitude + 1)
                      : static_cast<std::int64_t>(*magnitude);
    return 1;
}

std::int32_t fb_parse_uint(const fb_string_t* text, const std::int32_t base, std::uint64_t* value) {
    bool negative = false;
    const std::optional<std::uint64_t> magnitude = magnitude_of(view(text), base, negative);
    if (!magnitude || negative) {
        return 0;
    }

    *value = *magnitude;
    return 1;
}

std::int32_t fb_parse_float(const fb_string_t* text, const std::int32_t float32, double* value) {
    std::string_view digits = view(text);
    bool negative = false;
    if (!digits.empty() && (digits.front() == '+' || digits.front() == '-')) {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }

    // What Format writes for the floats that aren't numbers, and nothing else made of letters.
    if (digits == "NaN" || digits == "Inf") {
        const double special = digits == "NaN" ? std::numeric_limits<double>::quiet_NaN()
                                               : std::numeric_limits<double>::infinity();
        *value = negative ? -special : special;
        return digits == "NaN" && negative ? 0 : 1;
    }

    if (digits.empty() ||
        (digits.front() != '.' && (digits.front() < '0' || digits.front() > '9'))) {
        return 0;
    }

    const char* first = digits.data();
    const char* end = first + digits.size();
    if (float32 != 0) {
        float number = 0;
        const fast_float::from_chars_result result = fast_float::from_chars(first, end, number);
        if (result.ec != std::errc{} || result.ptr != end) {
            return 0;
        }

        *value = negative ? -static_cast<double>(number) : static_cast<double>(number);
        return 1;
    }

    double number = 0;
    const fast_float::from_chars_result result = fast_float::from_chars(first, end, number);
    if (result.ec != std::errc{} || result.ptr != end) {
        return 0;
    }

    *value = negative ? -number : number;
    return 1;
}

std::int32_t fb_format_split(const fb_string_t* text, fb_array_t* texts, fb_array_t* indexes,
                             fb_array_t* specs) {
    const std::string_view source = view(text);
    std::string piece;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if ((c == '{' || c == '}') && i + 1 < source.size() && source[i + 1] == c) {
            piece += c;
            ++i;
            continue;
        }

        if (c == '}') {
            return 2;
        }

        if (c != '{') {
            piece += c;
            continue;
        }

        // A field: its index, then its spec after a ':'.
        const std::size_t close = source.find('}', i);
        if (close == std::string_view::npos) {
            return 1;
        }

        const std::string_view field = source.substr(i + 1, close - i - 1);
        const std::size_t colon = std::min(field.find(':'), field.size());
        const std::string_view number = field.substr(0, colon);
        std::int32_t index = -1;
        if (!number.empty()) {
            const std::from_chars_result read =
                std::from_chars(number.data(), number.data() + number.size(), index);
            if (read.ec != std::errc{} || read.ptr != number.data() + number.size() || index < 0) {
                return 3;
            }
        }

        if (field.substr(colon).contains('{')) {
            return 4;
        }

        add_text(texts, piece);
        piece.clear();
        *static_cast<std::int32_t*>(fb_array_push(indexes, sizeof(std::int32_t))) = index;
        add_text(specs, std::string(colon < field.size() ? field.substr(colon + 1) : ""));
        i = close;
    }

    add_text(texts, piece);
    return 0;
}

} // extern "C"
