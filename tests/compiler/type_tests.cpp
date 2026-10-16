#include "fb/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string_view>

TEST_CASE("a type table starts with the built-ins at their own ids", "[types]") {
    const fb::type_table_t types;
    CHECK(types.size() == static_cast<std::size_t>(fb::type_id_t::BUILTIN_COUNT));
    CHECK(types.name(fb::type_id_t::INT32) == "Int32");
    CHECK(types.name(fb::type_id_t::STRING) == "String");
    CHECK(types.name(fb::type_id_t::NONE) == "nothing");
    CHECK(types.get(fb::type_id_t::BOOL).kind == fb::type_kind_t::BUILTIN);
}

TEST_CASE("built-in names and aliases", "[types]") {
    CHECK(fb::builtin_name(fb::type_id_t::UINT8) == "UInt8");
    CHECK(fb::builtin_type("Byte") == fb::type_id_t::UINT8);
    CHECK(fb::builtin_type("Long") == fb::type_id_t::INT64);
    CHECK(fb::builtin_type("Double") == fb::type_id_t::FLOAT64);
    CHECK(fb::builtin_type("Enemy") == std::nullopt);

    // Only the exact spelling is a type.
    CHECK(fb::builtin_type("int") == std::nullopt);
}

TEST_CASE("nullable and array types are made once and shared", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t maybe_int = types.nullable(fb::type_id_t::INT32);
    CHECK(maybe_int == fb::type_id_t::BUILTIN_COUNT);
    CHECK(types.nullable(fb::type_id_t::INT32) == maybe_int);
    CHECK(types.name(maybe_int) == "Int32?");
    CHECK(types.get(maybe_int).kind == fb::type_kind_t::NULLABLE);
    CHECK(types.get(maybe_int).inner == fb::type_id_t::INT32);

    const fb::type_id_t names = types.array(fb::type_id_t::STRING);
    CHECK(names != maybe_int);
    CHECK(types.name(names) == "String[]");
    CHECK(types.name(types.nullable(names)) == "String[]?");
    CHECK(types.name(types.array(maybe_int)) == "Int32?[]");
    CHECK(types.size() == static_cast<std::size_t>(fb::type_id_t::BUILTIN_COUNT) + 4);
}

TEST_CASE("names stay valid as types are added", "[types]") {
    fb::type_table_t types;
    const std::string_view first = types.name(types.array(fb::type_id_t::INT8));
    for (int i = 0; i < 100; ++i) {
        fb::type_id_t nested = fb::type_id_t::INT8;
        for (int depth = 0; depth <= i; ++depth) {
            nested = types.array(nested);
        }
    }

    CHECK(first == "Int8[]");
}

TEST_CASE("types made from an invalid type stay invalid", "[types]") {
    fb::type_table_t types;
    CHECK(types.nullable(fb::type_id_t::INVALID) == fb::type_id_t::INVALID);
    CHECK(types.array(fb::type_id_t::INVALID) == fb::type_id_t::INVALID);
    CHECK(types.size() == static_cast<std::size_t>(fb::type_id_t::BUILTIN_COUNT));
}

TEST_CASE("lossless conversions between numbers", "[types]") {
    using fb::type_id_t;
    CHECK(fb::converts_losslessly(type_id_t::INT32, type_id_t::INT64));
    CHECK(fb::converts_losslessly(type_id_t::UINT8, type_id_t::INT16));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::INT8, type_id_t::UINT64));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::UINT32, type_id_t::INT32));

    // Float32 has 24 bits of mantissa and Float64 53, so 16 and 32 bits are the widest that fit.
    CHECK(fb::converts_losslessly(type_id_t::INT16, type_id_t::FLOAT32));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::INT32, type_id_t::FLOAT32));
    CHECK(fb::converts_losslessly(type_id_t::UINT32, type_id_t::FLOAT64));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::INT64, type_id_t::FLOAT64));
    CHECK(fb::converts_losslessly(type_id_t::FLOAT32, type_id_t::FLOAT64));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::FLOAT64, type_id_t::FLOAT32));
}

TEST_CASE("a T? has its T inside, and Null for a zero", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t enemy = types.add_class("Enemy");
    const fb::type_id_t pair = types.add_struct("Pair");
    types.set_fields(pair, { { .name = "first", .type = enemy } });
    const fb::type_id_t maybe = types.nullable(enemy);
    REQUIRE(types.get(maybe).fields.size() == 1);
    CHECK(types.get(maybe).fields[0].type == enemy);
    CHECK(types.has_references(maybe));
    CHECK_FALSE(types.has_references(types.nullable(fb::type_id_t::INT32)));

    // A Class can't be Null, so it has no zero, and neither does a Struct holding one.
    CHECK_FALSE(types.has_zero(enemy));
    CHECK_FALSE(types.has_zero(pair));
    CHECK(types.has_zero(maybe));
    CHECK(types.has_zero(types.nullable(pair)));
    CHECK(types.has_zero(fb::type_id_t::STRING));
}
