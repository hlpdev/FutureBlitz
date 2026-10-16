#pragma once

#include "fb/source.hpp"
#include "fb/token.hpp"
#include "fb/types.hpp"

#include <catch2/catch_tostring.hpp>
#include <format>
#include <string>

// Readable values in Catch2 failure messages.

/// Shows the 0-based line and column as they are, not as diagnostics show them.
template <>
struct Catch::StringMaker<fb::location_t> {
    static std::string convert(const fb::location_t& location) {
        return std::format("{}:{}", location.line, location.column);
    }
};

template <>
struct Catch::StringMaker<fb::type_id_t> {
    static std::string convert(const fb::type_id_t type) {
        return std::string(fb::type_name(type));
    }
};

template <>
struct Catch::StringMaker<fb::token_kind_t> {
    static std::string convert(const fb::token_kind_t kind) {
        return std::string(fb::describe(kind));
    }
};
