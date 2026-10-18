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

TEST_CASE("a Class extends the Classes above it", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t shape = types.add_class("Shape");
    const fb::type_id_t rect = types.add_class("Rect");
    const fb::type_id_t square = types.add_class("Square");
    types.set_base(rect, shape);
    types.set_base(square, rect);
    CHECK(types.get(square).base == rect);
    CHECK(types.extends(square, shape));
    CHECK(types.extends(square, square));
    CHECK_FALSE(types.extends(shape, rect));
}

TEST_CASE("fixed arrays are made once and shared", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t grid = types.fixed_array(fb::type_id_t::FLOAT32, { 64, 32 });
    CHECK(types.fixed_array(fb::type_id_t::FLOAT32, { 64, 32 }) == grid);
    CHECK(types.fixed_array(fb::type_id_t::FLOAT32, { 32, 64 }) != grid);
    CHECK(types.name(grid) == "Float32[64, 32]");
    CHECK(types.element_count(grid) == 2048);
    CHECK(types.fixed_array(fb::type_id_t::INVALID, { 2 }) == fb::type_id_t::INVALID);

    // A T[] is a reference with no zero; a fixed array is as its elements are.
    const fb::type_id_t list = types.array(fb::type_id_t::INT32);
    CHECK(types.has_references(list));
    CHECK_FALSE(types.has_zero(list));
    CHECK_FALSE(types.has_references(grid));
    CHECK(types.has_zero(grid));
    CHECK(types.has_references(types.fixed_array(fb::type_id_t::STRING, { 2 })));
}

TEST_CASE("an Enum's zero is its first value, or no flags", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t state = types.add_enum("State");
    types.set_values(state, fb::type_id_t::UINT8,
                     { { .name = "Idle", .bits = 5 }, { .name = "Walk", .bits = 6 } }, false);
    CHECK(types.get(state).inner == fb::type_id_t::UINT8);
    CHECK(types.enum_zero(state) == 5);
    CHECK(types.has_zero(state));
    CHECK_FALSE(types.has_references(state));

    const fb::type_id_t layer = types.add_enum("Layer");
    types.set_values(layer, fb::type_id_t::INT32, { { .name = "World", .bits = 1 } }, true);
    CHECK(types.enum_zero(layer) == 0);
}

TEST_CASE("a Weak<T> is made once, along with the T? that reading it gives", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t node = types.add_class("Node");
    CHECK(types.find_nullable(node) == fb::type_id_t::INVALID);
    const fb::type_id_t weak = types.weak(node);
    CHECK(types.name(weak) == "Weak<Node>");
    CHECK(types.weak(node) == weak);
    CHECK(types.get(weak).inner == node);
    CHECK(types.find_nullable(node) == types.nullable(node));

    // It holds a weak reference, and its zero refers to nothing.
    CHECK(types.has_references(weak));
    CHECK(types.has_zero(weak));
    CHECK(types.weak(fb::type_id_t::INVALID) == fb::type_id_t::INVALID);
}

TEST_CASE("a tuple is made once for its element types, which are its fields", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t pair = types.tuple({ fb::type_id_t::INT32, fb::type_id_t::STRING });
    CHECK(types.name(pair) == "(Int32, String)");
    CHECK(types.get(pair).kind == fb::type_kind_t::TUPLE);
    CHECK(types.tuple({ fb::type_id_t::INT32, fb::type_id_t::STRING }) == pair);
    CHECK(types.tuple({ fb::type_id_t::STRING, fb::type_id_t::INT32 }) != pair);
    REQUIRE(types.get(pair).fields.size() == 2);
    CHECK(types.get(pair).fields[1].name == "1");
    CHECK(types.get(pair).fields[1].type == fb::type_id_t::STRING);
    CHECK(types.name(types.tuple({ pair, fb::type_id_t::BOOL })) == "((Int32, String), Bool)");

    // It holds the String's reference, and its zero is each element's. A Class has no zero, so
    // neither does a tuple holding one.
    CHECK(types.has_references(pair));
    CHECK(types.has_zero(pair));
    const fb::type_id_t node = types.add_class("Node");
    CHECK_FALSE(types.has_zero(types.tuple({ node, fb::type_id_t::INT32 })));
    CHECK(types.tuple({ fb::type_id_t::INVALID, fb::type_id_t::INT32 }) == fb::type_id_t::INVALID);
}

TEST_CASE("a Func type is made once for its parameters and result", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t func =
        types.function({ fb::type_id_t::INT32, fb::type_id_t::STRING }, fb::type_id_t::BOOL);
    CHECK(types.name(func) == "Func(Int32, String): Bool");
    CHECK(types.get(func).kind == fb::type_kind_t::FUNCTION);
    CHECK(types.function({ fb::type_id_t::INT32, fb::type_id_t::STRING }, fb::type_id_t::BOOL) ==
          func);
    CHECK(types.name(types.function({}, fb::type_id_t::NONE)) == "Func()");

    // A function value refers to an object, and can't be Null, so it has no zero. One that
    // returns something goes in parentheses before a '?' or '[]'.
    CHECK(types.has_references(func));
    CHECK_FALSE(types.has_zero(func));
    CHECK(types.name(types.nullable(func)) == "(Func(Int32, String): Bool)?");
    CHECK(types.name(types.array(types.function({}, fb::type_id_t::NONE))) == "Func()[]");
    CHECK(types.function({ fb::type_id_t::INVALID }, fb::type_id_t::NONE) ==
          fb::type_id_t::INVALID);
}

TEST_CASE("a generic type's instances get its fields with their type arguments", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t t = types.add_parameter("T");
    const fb::type_id_t box = types.add_struct("Box<T>");
    types.set_parameters(box, { t });

    // An instance made before the fields are known gets them once they are.
    const fb::type_id_t ints = types.instance(box, { fb::type_id_t::INT32 });
    CHECK(types.name(ints) == "Box<Int32>");
    types.set_fields(box, { { .name = "Value", .type = types.nullable(t) } });
    REQUIRE(types.get(ints).fields.size() == 1);
    CHECK(types.get(ints).fields[0].type == types.nullable(fb::type_id_t::INT32));
    CHECK(types.instance(box, { fb::type_id_t::INT32 }) == ints);
    CHECK(types.get(ints).generic == box);

    // With its own parameter, it's the generic type itself.
    CHECK(types.instance(box, { t }) == box);
    CHECK(types.substitute(types.array(box), { t }, { fb::type_id_t::STRING }) ==
          types.array(types.instance(box, { fb::type_id_t::STRING })));

    // T? with an Int32? for T is Int32?, not Int32??.
    const fb::type_id_t maybe = types.nullable(fb::type_id_t::INT32);
    CHECK(types.substitute(types.nullable(t), { t }, { maybe }) == maybe);
    CHECK_FALSE(types.has_zero(t));
}

TEST_CASE("a copy of a type table keeps its types and adds its own", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t point = types.add_struct("Point");
    fb::type_table_t copy = types;
    CHECK(copy.name(point) == "Point");
    const fb::type_id_t points = copy.array(point);
    CHECK(copy.name(points) == "Point[]");
    CHECK(types.size() < copy.size());
}

TEST_CASE("an Interface has a Self, and extends the Interfaces it lists", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t shown = types.add_interface("Shown");
    const fb::type_id_t item = types.add_interface("Item");
    types.set_interfaces(item, { shown });
    CHECK(types.name(item) == "Item");
    CHECK(types.get(types.get(item).inner).kind == fb::type_kind_t::PARAMETER);
    CHECK(types.get(item).inner != types.get(shown).inner);
    CHECK(types.extends(item, shown));
    CHECK(types.extends(item, item));
    CHECK_FALSE(types.extends(shown, item));

    // Its values are references to objects, which can't be Null.
    CHECK(types.has_references(item));
    CHECK_FALSE(types.has_zero(item));
    CHECK(types.has_zero(types.nullable(item)));

    // A generic Class's instances implement what it does.
    const fb::type_id_t t = types.add_parameter("T");
    const fb::type_id_t box = types.add_class("Box<T>");
    types.set_parameters(box, { t });
    const fb::type_id_t ints = types.instance(box, { fb::type_id_t::INT32 });
    types.set_interfaces(box, { item });
    CHECK(types.get(ints).interfaces == std::vector{ item });
}

TEST_CASE("a generic Interface's instances share its Self, and extend instances", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t t = types.add_parameter("T");
    const fb::type_id_t source = types.add_interface("Source<T>");
    types.set_parameters(source, { t });
    const fb::type_id_t sink = types.add_interface("Sink<T>");
    types.set_parameters(sink, { t });
    types.set_interfaces(sink, { types.instance(source, { types.array(t) }) });

    const fb::type_id_t ints = types.instance(sink, { fb::type_id_t::INT32 });
    CHECK(types.name(ints) == "Sink<Int32>");
    CHECK(types.get(ints).inner == types.get(sink).inner);
    CHECK(types.name(types.get(ints).interfaces.front()) == "Source<Int32[]>");
    CHECK(types.instance(sink, { fb::type_id_t::INT32 }) == ints);
    CHECK(types.substitute(sink, { t }, { fb::type_id_t::INT32 }) == ints);
}

TEST_CASE("a Const type parameter's arguments are values, which fixed arrays take as sizes",
          "[types]") {
    fb::type_table_t types;
    const fb::type_id_t w = types.add_value_parameter("W", fb::type_id_t::INT32);
    CHECK(types.get(w).inner == fb::type_id_t::INT32);
    const fb::type_id_t four = types.value(fb::type_id_t::INT32, 4);
    CHECK(types.name(four) == "4");
    CHECK(types.value(fb::type_id_t::INT32, 4) == four);

    const fb::type_id_t cells = types.fixed_array(fb::type_id_t::FLOAT32, { 0, 2 }, { w });
    CHECK(types.name(cells) == "Float32[W, 2]");
    const fb::type_id_t made = types.substitute(cells, { w }, { four });
    CHECK(made == types.fixed_array(fb::type_id_t::FLOAT32, { 4, 2 }));
    CHECK(types.name(made) == "Float32[4, 2]");
}
