#pragma once

#include <cstddef>
#include <span>
#include <string_view>

// The IANA tz database, which third_party/ compiles with its zic and embeds. tz_data.cpp, which
// the build makes, has each zone's TZif data (RFC 8536) one after another, and each name, of a
// zone or of a link to one, sorted, with where its zone's data is.
namespace fb::tz {

struct entry_t {
    const char* name;
    std::size_t offset;
    std::size_t size;
};

std::span<const unsigned char> data();
std::span<const entry_t> entries();
std::string_view version();

} // namespace fb::tz
