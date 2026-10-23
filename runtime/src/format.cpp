#include "fb/runtime.hpp"
#include "fb/text_format.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

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

} // extern "C"
