#include "fb/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
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

TEST_CASE("C's integers convert losslessly where every target's would", "[types]") {
    using fb::type_id_t;
    // A CLong is an Int32 or an Int64, a CULong a UInt32 or a UInt64, and a CWChar a UInt16, an
    // Int32, or a UInt32.
    CHECK(fb::converts_losslessly(type_id_t::INT32, type_id_t::CLONG));
    CHECK(fb::converts_losslessly(type_id_t::UINT16, type_id_t::CLONG));
    CHECK(fb::converts_losslessly(type_id_t::CLONG, type_id_t::INT64));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::INT64, type_id_t::CLONG));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::UINT32, type_id_t::CLONG));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::CLONG, type_id_t::INT32));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::CLONG, type_id_t::FLOAT64));
    CHECK(fb::converts_losslessly(type_id_t::UINT32, type_id_t::CULONG));
    CHECK(fb::converts_losslessly(type_id_t::CULONG, type_id_t::UINT64));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::CULONG, type_id_t::INT64));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::CLONG, type_id_t::CULONG));
    CHECK(fb::converts_losslessly(type_id_t::UINT16, type_id_t::CWCHAR));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::INT16, type_id_t::CWCHAR));
    CHECK(fb::converts_losslessly(type_id_t::CWCHAR, type_id_t::INT64));
    CHECK(fb::converts_losslessly(type_id_t::CWCHAR, type_id_t::FLOAT64));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::CWCHAR, type_id_t::INT32));
    CHECK_FALSE(fb::converts_losslessly(type_id_t::CWCHAR, type_id_t::UINT32));
    // Each target's wchar_t fits in its long.
    CHECK(fb::converts_losslessly(type_id_t::CWCHAR, type_id_t::CLONG));
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

    // T? with an Int32? for T is Int32??, which holds an Int32? that can be Null itself.
    const fb::type_id_t maybe = types.nullable(fb::type_id_t::INT32);
    const fb::type_id_t twice = types.substitute(types.nullable(t), { t }, { maybe });
    CHECK(twice == types.nullable(maybe));
    CHECK(types.name(twice) == "Int32??");
    CHECK_FALSE(types.has_zero(t));
}

TEST_CASE("a copy of a type table keeps its types and adds its own", "[types]") {
    fb::type_table_t types;
    const fb::type_id_t point = types.add_struct("Point");
    const fb::type_id_t callback = types.callback({ fb::type_id_t::INT32 }, fb::type_id_t::INT32);
    const fb::type_id_t pointer = types.pointer(callback);
    fb::type_table_t copy = types;
    CHECK(copy.name(point) == "Point");
    const fb::type_id_t points = copy.array(point);
    CHECK(copy.name(points) == "Point[]");
    CHECK(types.size() < copy.size());

    // It makes the Callbacks the table has as the table would, not new ones.
    CHECK(copy.callback({ fb::type_id_t::INT32 }, fb::type_id_t::INT32) == callback);
    CHECK(copy.name(copy.array(callback)) == "(Callback(Int32): Int32)[]");

    // And the Ptrs too, so a generic type's instance over one is one type.
    CHECK(copy.pointer(callback) == pointer);
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

    // One instance keeps what an Extend of it alone lists when the generic type's fields, base,
    // or Interfaces are filled in again.
    types.set_interfaces(ints, { item, shown });
    types.set_fields(box, { { .name = "Value", .type = t } });
    const fb::type_id_t other = types.add_interface("Other");
    types.set_interfaces(box, { item, other });
    CHECK(types.get(ints).interfaces == std::vector{ item, other, shown });
    CHECK(types.get(types.instance(box, { fb::type_id_t::STRING })).interfaces ==
          std::vector{ item, other });
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
    // A value is an Int64's, the same type for the same number whatever its parameter's type.
    const fb::type_id_t four = types.value(4);
    CHECK(types.name(four) == "4");
    CHECK(types.value(4) == four);
    CHECK(types.get(four).inner == fb::type_id_t::INT64);

    const fb::type_id_t cells = types.fixed_array(fb::type_id_t::FLOAT32, { 0, 2 }, { w });
    CHECK(types.name(cells) == "Float32[W, 2]");
    const fb::type_id_t made = types.substitute(cells, { w }, { four });
    CHECK(made == types.fixed_array(fb::type_id_t::FLOAT32, { 4, 2 }));
    CHECK(types.name(made) == "Float32[4, 2]");
}

TEST_CASE("values computed alike are one value", "[types]") {
    using enum fb::value_op_t;
    fb::type_table_t types;
    const fb::type_id_t l = types.add_value_parameter("L", fb::type_id_t::INT32);
    const fb::type_id_t m = types.add_value_parameter("M", fb::type_id_t::INT8);
    const auto known = [&types](const std::int64_t number) { return types.value(number); };

    // + and -, and * by a known number, combine, whatever order they're written in.
    CHECK(types.value_expression(SUBTRACT, l, known(0)) == l);
    CHECK(types.value_expression(MULTIPLY, known(1), l) == l);
    CHECK(types.value_expression(DIVIDE, l, known(1)) == l);
    const fb::type_id_t twice = types.value_expression(MULTIPLY, l, known(2));
    CHECK(types.name(twice) == "L * 2");
    CHECK(types.value_expression(ADD, l, l) == twice);
    CHECK(types.value_expression(MULTIPLY, known(2), l) == twice);
    const fb::type_id_t both = types.value_expression(ADD, m, l);
    CHECK(types.name(both) == "L + M");
    CHECK(types.value_expression(ADD, l, m) == both);
    CHECK(types.value_expression(SUBTRACT, types.value_expression(ADD, l, known(1)),
                                 types.value_expression(SUBTRACT, l, known(2))) == known(3));
    CHECK(types.value_expression(SUBTRACT, l, l) == known(0));
    CHECK(types.value_expression(MULTIPLY, l, known(0)) == known(0));

    // The parts it adds come first, then those it subtracts, then the known number.
    const fb::type_id_t mixed = types.value_expression(
        ADD,
        types.value_expression(SUBTRACT, known(1), types.value_expression(MULTIPLY, m, known(3))),
        twice);
    CHECK(types.name(mixed) == "L * 2 - M * 3 + 1");
    CHECK(types.name(types.value_expression(SUBTRACT, known(0), l)) == "0 - L");
    CHECK(types.name(types.value_expression(SUBTRACT, l, known(-1))) == "L + 1");

    // Each instance works out what it combines to.
    CHECK(types.substitute(mixed, { l, m }, { known(5), known(2) }) == known(5));

    // One that would drop a part that can fail, or pass what an Int64 holds, stays as it's
    // written.
    const fb::type_id_t split = types.value_expression(DIVIDE, known(10), l);
    CHECK(types.name(types.value_expression(MULTIPLY, split, known(0))) == "10 / L * 0");
    CHECK(types.name(types.value_expression(SUBTRACT, split, split)) == "10 / L - 10 / L");
    CHECK(types.name(types.value_expression(ADD, split, split)) == "10 / L * 2");
    const fb::type_id_t big = types.value_expression(MULTIPLY, l, known(4611686018427387904));
    CHECK(types.name(types.value_expression(MULTIPLY, big, known(4))) ==
          "L * 4611686018427387904 * 4");
    CHECK(types.name(types.value_expression(MULTIPLY, l, l)) == "L * L");
}

TEST_CASE("an instance has its generic type's module, and the compiler's Classes have none",
          "[types]") {
    fb::type_table_t types;
    const fb::type_id_t t = types.add_parameter("T");
    const fb::type_id_t box = types.add_struct("Box<T>", "lib.a");
    types.set_parameters(box, { t });
    const fb::type_id_t ints = types.instance(box, { fb::type_id_t::INT32 });
    CHECK(types.get(ints).module == "lib.a");
    CHECK(types.qualified_name(ints, "main") == "lib.a.Box<Int32>");

    // A captured Local's Cell<T> is the compiler's, so only what's inside has a module.
    const fb::type_id_t value = types.add_parameter("T");
    const fb::type_id_t cell = types.add_class("Cell<T>");
    types.set_made(cell);
    types.set_parameters(cell, { value });
    const fb::type_id_t node = types.add_class("Node");
    CHECK(types.qualified_name(types.instance(cell, { node }), "main") == "Cell<main.Node>");
}
