#include "fb/driver.hpp"
#include "fb/proto.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using diagnostics_t = std::vector<std::string>;

/// "line:column: message" for a diagnostic, from 1, as its file has them, after "warning: " for
/// a warning, and its file's path for `with_path`.
std::string shown(const fb::source_manager_t& sources, const fb::diagnostic_t& diagnostic,
                  const bool with_path) {
    const fb::source_file_t& file = sources.get(diagnostic.span.file);
    const fb::location_t at = file.location_of(diagnostic.span.begin, fb::column_unit_t::CODEPOINT);
    return std::format(
        "{}{}{}:{}: {}", diagnostic.severity == fb::severity_t::WARNING ? "warning: " : "",
        with_path ? file.path() + ":" : "", at.line + 1, at.column + 1, diagnostic.message);
}

struct parsed_t {
    fb::proto_file_t file;
    diagnostics_t diagnostics;
};

/// Reads `text` as a .proto file.
parsed_t parse(const std::string_view text) {
    fb::source_manager_t sources;
    const fb::file_id_t id = sources.add("test.proto", std::string(text));
    fb::diagnostic_bag_t bag;
    parsed_t parsed{ .file = fb::parse_proto(sources.get(id), bag) };
    for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
        parsed.diagnostics.push_back(shown(sources, diagnostic, false));
    }

    return parsed;
}

/// The errors of reading `text` as a .proto file.
diagnostics_t parse_errors(const std::string_view text) {
    return parse(text).diagnostics;
}

/// Reads each of `files`, (path, text) pairs, whose imports name each other by those paths, and
/// resolves the first: "path:line:column: message" for each diagnostic.
diagnostics_t resolve(const std::vector<std::pair<std::string, std::string>>& files) {
    fb::source_manager_t sources;
    fb::diagnostic_bag_t bag;
    std::vector<std::unique_ptr<fb::proto_file_t>> parsed;
    for (const auto& [path, text] : files) {
        const fb::file_id_t id = sources.add(path, text);
        parsed.push_back(std::make_unique<fb::proto_file_t>(fb::parse_proto(sources.get(id), bag)));
        parsed.back()->path = path;
    }

    for (const std::unique_ptr<fb::proto_file_t>& file : parsed) {
        for (fb::proto_import_t& imported : file->imports) {
            const auto found = std::ranges::find(parsed, imported.path,
                                                 [](const auto& other) { return other->path; });
            imported.file = found != parsed.end() ? found->get() : nullptr;
        }
    }

    fb::resolve_proto(*parsed.front(), bag);
    diagnostics_t shown_diagnostics;
    for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
        shown_diagnostics.push_back(shown(sources, diagnostic, true));
    }

    return shown_diagnostics;
}

/// The errors of resolving `text` as a .proto file alone.
diagnostics_t resolve_errors(const std::string_view text) {
    return resolve({ { "test.proto", std::string(text) } });
}

/// main.fb's `text` with `files`, by their paths from its folder, and the standard library.
fb::compilation_t compile_with(std::string text, std::vector<fb::library_file_t> files) {
    std::ranges::move(fb::standard_library().value_or(std::vector<fb::library_file_t>{}),
                      std::back_inserter(files));
    return fb::compile("main.fb", std::move(text), std::move(files));
}

/// "path:line:column: message" for each diagnostic of a compilation.
diagnostics_t diagnostics_of(const fb::compilation_t& compilation) {
    diagnostics_t shown_diagnostics;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        shown_diagnostics.push_back(shown(compilation.sources, diagnostic, true));
    }

    return shown_diagnostics;
}

/// The module fb made from the .proto file at `path` in a compilation.
std::string generated(const fb::compilation_t& compilation, const std::string_view path) {
    for (std::size_t i = 0; i < compilation.sources.size(); ++i) {
        const fb::source_file_t& file = compilation.sources.get(static_cast<fb::file_id_t>(i));
        if (file.path() == std::format("{}.fb", path)) {
            return std::string(file.text());
        }
    }

    return {};
}

} // namespace

TEST_CASE("a proto3 file's package, imports, messages, enums, and services", "[proto]") {
    const parsed_t parsed = parse(R"(// The address book.
syntax = "proto3";
package tutorial.book;
import "other.proto";
import public "shared.proto";
option java_package = "com.example";

message Person {
  string name = 1;
  optional int32 id = 2;
  repeated int32 emails = 3 [packed = false, deprecated = true];
  map<string, Person> friends = 4;
  oneof contact {
    string phone = 5;
    Address address = 6;
  }
  message Address { string city = 1; }
  enum Kind { KIND_UNKNOWN = 0; KIND_CHILD = 1; }
  reserved 9, 10 to 12, 100 to max;
  reserved "old";
}

service Book {
  rpc Find(Person) returns (Person);
  rpc Watch(stream Person) returns (stream .tutorial.book.Person) { option deprecated = true; }
}
)");
    CHECK(parsed.diagnostics.empty());
    const fb::proto_file_t& file = parsed.file;
    CHECK(file.syntax == fb::proto_syntax_t::PROTO3);
    CHECK(file.package == "tutorial.book");
    REQUIRE(file.imports.size() == 2);
    CHECK(file.imports[0].path == "other.proto");
    CHECK(!file.imports[0].is_public);
    CHECK(file.imports[1].is_public);

    REQUIRE(file.messages.size() == 2);
    const fb::proto_message_t& person = file.messages[0];
    CHECK(person.full_name == "tutorial.book.Person");
    CHECK(file.messages[1].full_name == "tutorial.book.Person.Address");
    CHECK(file.messages[1].parent == 0);
    REQUIRE(person.fields.size() == 6);
    CHECK(person.fields[0].label == fb::proto_label_t::NONE);
    CHECK(person.fields[0].type.kind == fb::proto_kind_t::STRING);
    CHECK(person.fields[1].label == fb::proto_label_t::OPTIONAL);
    CHECK(person.fields[2].label == fb::proto_label_t::REPEATED);
    CHECK(person.fields[2].packed == false);
    CHECK(person.fields[2].deprecated);
    CHECK(person.fields[3].key.value_or(fb::proto_type_t{}).kind == fb::proto_kind_t::STRING);
    CHECK(person.fields[3].type.name == "Person");
    CHECK(person.fields[4].oneof == 0);
    CHECK(person.fields[5].type.name == "Address");
    CHECK(person.oneofs.front().name == "contact");
    REQUIRE(person.reserved.size() == 3);
    CHECK(person.reserved[1].low == 10);
    CHECK(person.reserved[1].high == 12);
    CHECK(person.reserved[2].high == 536870911);
    CHECK(person.reserved_names == std::vector<std::string>{ "old" });

    REQUIRE(file.enums.size() == 1);
    CHECK(file.enums[0].full_name == "tutorial.book.Person.Kind");
    CHECK(file.enums[0].values[1].number == 1);

    REQUIRE(file.services.size() == 1);
    const fb::proto_service_t& book = file.services[0];
    REQUIRE(book.methods.size() == 2);
    CHECK(!book.methods[0].client_streaming);
    CHECK(book.methods[1].client_streaming);
    CHECK(book.methods[1].server_streaming);
    CHECK(book.methods[1].output.name == ".tutorial.book.Person");
    CHECK(book.methods[1].deprecated);
}

TEST_CASE("proto2's labels, defaults, groups, extensions, and extends", "[proto]") {
    const parsed_t parsed = parse(R"(syntax = "proto2";
message Old {
  required int32 id = 1 [default = -12];
  optional double ratio = 2 [default = -inf];
  optional float scale = 3 [default = 1.5e3];
  optional string name = 4 [default = "a\tb" 'c\x41\101\u00e9\U0001F600'];
  optional uint64 mask = 5 [default = 0x7f];
  optional uint32 mode = 6 [default = 0755];
  repeated group Result = 7 {
    optional string url = 8;
  }
  extensions 100 to 199, 500;
  extend Old { optional int32 extra = 150; }
}
extend Old { repeated group Bonus = 160 { enum E { A = 0; } optional E x = 1; } }
message After { optional int32 y = 1; }
)");
    CHECK(parsed.diagnostics.empty());
    const fb::proto_file_t& file = parsed.file;
    CHECK(file.syntax == fb::proto_syntax_t::PROTO2);
    // Extends' fields, and their groups' messages and enums, are left out.
    REQUIRE(file.messages.size() == 3);
    CHECK(file.enums.empty());
    CHECK(file.messages[2].full_name == "After");
    const fb::proto_message_t& old = file.messages[0];
    REQUIRE(old.fields.size() == 7);
    const auto default_of = [&](const std::size_t field) {
        return old.fields[field].default_value.value_or(fb::proto_constant_t{});
    };
    CHECK(old.fields[0].label == fb::proto_label_t::REQUIRED);
    CHECK(default_of(0).negative);
    CHECK(default_of(0).integer == 12);
    CHECK(default_of(1).kind == fb::proto_constant_kind_t::FLOAT);
    CHECK(default_of(1).text == "inf");
    CHECK(default_of(2).number == 1500);
    CHECK(default_of(3).text == "a\tbcAA\u00e9\U0001F600");
    CHECK(default_of(4).integer == 127);
    CHECK(default_of(5).integer == 493);

    const fb::proto_field_t& result = old.fields[6];
    CHECK(result.group);
    CHECK(result.name == "result");
    CHECK(result.type.name == "Result");
    CHECK(file.messages[1].group);
    CHECK(file.messages[1].full_name == "Old.Result");
    CHECK(file.messages[1].fields.front().name == "url");
    REQUIRE(old.extensions.size() == 2);
    CHECK(old.extensions[1].low == 500);
    CHECK(old.extensions[1].high == 500);
}

TEST_CASE("a .proto's syntax errors are at their lines and columns", "[proto]") {
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M {\n  int32 x = 1\n}\n") ==
          diagnostics_t{ "4:1: expected ';', found '}'" });
    CHECK(parse_errors("syntax = \"proto3\"\nmessage M {}\n") ==
          diagnostics_t{ "2:1: expected ';', found 'message'" });
    CHECK(parse_errors("syntax = \"proto4\";") ==
          diagnostics_t{ "1:10: the syntax is \"proto2\" or \"proto3\", not \"proto4\"" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M {\n  string s = 1 [default = \"x];\n}\n") ==
          diagnostics_t{
              "3:27: a string needs its closing \" on its line",
              "4:1: expected ',' or ']', found '}'",
              "3:17: proto3's fields have no defaults: each one's is its zero",
          });
    CHECK(parse_errors("syntax = \"proto3\";\n/* never closed") ==
          diagnostics_t{ "2:1: a /* comment needs its */" });
    CHECK(parse_errors(
              "syntax = \"proto2\";\nmessage M { optional string s = 1 [default = \"\\q\"]; }") ==
          diagnostics_t{ "2:47: '\\q' isn't an escape a .proto's string has" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 12abc; }") ==
          diagnostics_t{ "2:23: '12abc' is a number with letters after it" });
    CHECK(parse_errors("syntax = \"proto3\";\nenum E { A = 0; B = 09; }") ==
          diagnostics_t{ "2:21: 09 isn't an octal number, which has only the digits 0 to 7" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 1; } $") ==
          diagnostics_t{ "2:28: '$' isn't part of a .proto's syntax" });
    CHECK(
        parse_errors("syntax = \"proto3\";\nmessage M {\n  int32 x = 1;\n") ==
        diagnostics_t{ "4:1: expected '}' at the end of the message, found the end of the file" });
    CHECK(parse_errors("syntax = \"proto3\";\nfoo bar;") ==
          diagnostics_t{
              "2:1: expected a declaration, like a message, an enum, or a service, found 'foo'",
          });
    CHECK(parse_errors("package a;\nsyntax = \"proto3\";") ==
          diagnostics_t{
              "warning: 1:1: a .proto file without a syntax line is proto2; say syntax = "
              "\"proto2\" or syntax = \"proto3\" first",
              "2:1: the syntax line comes first in a .proto file",
          });
    CHECK(parse_errors("edition = \"2023\";") ==
          diagnostics_t{ "1:1: editions aren't supported; a .proto file says syntax = \"proto2\" "
                         "or \"proto3\"" });
    CHECK(parse_errors("syntax = \"proto3\";\npackage a;\npackage b;") ==
          diagnostics_t{ "3:1: a .proto file has one package line" });
    CHECK(parse_errors("syntax = \"proto3\";\nimport \"a.proto\";\nimport \"a.proto\";") ==
          diagnostics_t{ "3:8: \"a.proto\" is imported already" });
    // After an error, the next statement still reads.
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M {\n  int32 = 1;\n  string = 2;\n}\n") ==
          diagnostics_t{ "3:9: expected the field's name, found '='",
                         "4:10: expected the field's name, found '='" });
    // A '}' that closes nothing is an error of its own, after which the file still reads: one
    // left over, or after a declaration without its '{'.
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 1; }\n}\nmessage N {}\n") ==
          diagnostics_t{ "3:1: this '}' closes no block" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M int32 x = 1; }\nenum E A = 0; }\n") ==
          diagnostics_t{
              "2:11: expected '{', found 'int32'",
              "2:24: this '}' closes no block",
              "3:8: expected '{', found 'A'",
              "3:15: this '}' closes no block",
          });
    // What isn't an option in an rpc's braces is skipped there, so the service goes on.
    const parsed_t service = parse("syntax = \"proto3\";\nmessage A {}\nservice S {\n"
                                   "  rpc F(A) returns (A) { foo; }\n  rpc G(A) returns (A);\n}\n");
    CHECK(service.diagnostics == diagnostics_t{ "4:26: expected an option, found 'foo'" });
    REQUIRE(service.file.services.size() == 1);
    CHECK(service.file.services.front().methods.size() == 2);
    // A run of characters that aren't tokens is read in a loop, each one an error.
    fb::source_manager_t sources;
    const fb::file_id_t junk = sources.add(
        "junk.proto", "syntax = \"proto3\";\n" + std::string(300000, '$') + "\nmessage M {}\n");
    fb::diagnostic_bag_t bag;
    const fb::proto_file_t junk_file = fb::parse_proto(sources.get(junk), bag);
    CHECK(bag.diagnostics().size() == 300000);
    CHECK(junk_file.messages.size() == 1);
}

TEST_CASE("a .proto's numbers and strings, at their edges", "[proto]") {
    // A \u head surrogate and a \u trail surrogate after it are one character, as protoc reads
    // them; either alone isn't one.
    const parsed_t pair = parse(
        R"(syntax = "proto2";
message M { optional string s = 1 [default = "\uD83D\uDE00"]; })");
    CHECK(pair.diagnostics.empty());
    REQUIRE(pair.file.messages.size() == 1);
    CHECK(pair.file.messages[0].fields[0].default_value.value_or(fb::proto_constant_t{}).text ==
          "\U0001F600");
    CHECK(parse_errors(R"(syntax = "proto2";
message M { optional string s = 1 [default = "\uD83Dx\uDE00"]; })") ==
          diagnostics_t{ R"(2:47: '\uD83D' isn't a character's code point)",
                         R"(2:54: '\uDE00' isn't a character's code point)" });
    // A decimal integer past 64 bits is a double's or a float's default, as protoc reads it, and
    // an error where an integer is needed; a hex or octal one is an error either way.
    const parsed_t big = parse("syntax = \"proto2\";\nmessage M {\n"
                               "  optional double d = 1 [default = 100000000000000000000];\n"
                               "  optional float f = 2 [default = -100000000000000000000];\n}\n");
    CHECK(big.diagnostics.empty());
    REQUIRE(big.file.messages.size() == 1);
    const fb::proto_constant_t d =
        big.file.messages[0].fields[0].default_value.value_or(fb::proto_constant_t{});
    CHECK(d.too_big);
    CHECK(d.number == 1e20);
    CHECK(resolve_errors("syntax = \"proto2\";\nmessage M {\n"
                         "  optional uint64 u = 1 [default = 18446744073709551616];\n"
                         "  optional double h = 2 [default = 0x10000000000000000];\n}\n") ==
          diagnostics_t{
              "test.proto:3:36: 18446744073709551616 doesn't fit in uint64, u's type",
              "test.proto:4:36: 0x10000000000000000 doesn't fit in 64 bits, as a hex or octal "
              "number has to",
          });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 100000000000000000000; }") ==
          diagnostics_t{ "2:23: a field's number is from 1 to 536870911, not "
                         "100000000000000000000" });
    // A custom option reads past one, as it reads past the option.
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M {\n"
                       "  int32 x = 1 [(weight) = 100000000000000000000];\n}\n")
              .empty());
    // A number that starts with 0 is octal, which is an integer.
    CHECK(
        parse_errors(
            "syntax = \"proto2\";\nmessage M { optional double d = 1 [default = 01.5]; }") ==
        diagnostics_t{ "2:46: 01.5 starts with 0, so it's an octal number, which is an integer" });
    CHECK(parse_errors("syntax = \"proto2\";\nmessage M { optional double d = 1 [default = 0.5]; }")
              .empty());
}

TEST_CASE("a package line puts what the file declares before it in the package too", "[proto]") {
    const parsed_t parsed = parse("syntax = \"proto3\";\nmessage A { B b = 1; enum E { X = 0; } }\n"
                                  "service S {}\npackage p;\nmessage B { int32 v = 1; }\n");
    CHECK(parsed.diagnostics.empty());
    REQUIRE(parsed.file.messages.size() == 2);
    CHECK(parsed.file.messages[0].full_name == "p.A");
    CHECK(parsed.file.messages[1].full_name == "p.B");
    CHECK(parsed.file.enums.front().full_name == "p.A.E");
    CHECK(parsed.file.services.front().full_name == "p.S");
    CHECK(resolve_errors("syntax = \"proto3\";\nmessage A { B b = 1; }\npackage p;\n"
                         "message B { int32 v = 1; }\n")
              .empty());
}

TEST_CASE("proto2 and proto3 each have their own rules", "[proto]") {
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { required int32 x = 1; }") ==
          diagnostics_t{ "2:13: proto3 has no required fields" });
    CHECK(parse_errors("syntax = \"proto2\";\nmessage M { int32 x = 1; }") ==
          diagnostics_t{ "2:13: a proto2 field starts with its label: required, optional, or "
                         "repeated" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 1 [default = 2]; }") ==
          diagnostics_t{ "2:26: proto3's fields have no defaults: each one's is its zero" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { repeated group G = 1 {} }") ==
          diagnostics_t{ "2:13: proto3 has no groups; use a message field" });
    CHECK(parse_errors("syntax = \"proto2\";\nmessage M { optional group g = 1 {} }") ==
          diagnostics_t{ "2:28: a group's name starts with a capital letter" });
    CHECK(parse_errors("syntax = \"proto3\";\nenum E { A = 1; }") ==
          diagnostics_t{ "2:10: a proto3 enum's first value is 0, its zero, which a field it "
                         "doesn't set has" });
    CHECK(parse_errors("syntax = \"proto2\";\nmessage M { repeated int32 x = 1 [default = 1]; }") ==
          diagnostics_t{ "2:35: a repeated field has no default" });
}

TEST_CASE("a message's field numbers and names", "[proto]") {
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 0; }") ==
          diagnostics_t{ "2:23: a field's number is from 1 to 536870911, not 0" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 536870912; }") ==
          diagnostics_t{ "2:23: a field's number is from 1 to 536870911, not 536870912" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 19500; }") ==
          diagnostics_t{ "2:23: field numbers 19000 to 19999 are protobuf's own" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 1; int32 y = 1; }") ==
          diagnostics_t{ "2:36: y has x's number, 1" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { reserved 2 to 4; int32 x = 3; }") ==
          diagnostics_t{ "2:40: x's number, 3, is reserved" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { reserved \"x\"; int32 x = 3; }") ==
          diagnostics_t{ "2:33: the name x is reserved" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { reserved x; }") ==
          diagnostics_t{ "2:22: a reserved name goes in quotes in proto2 and proto3: \"x\"" });
    CHECK(parse_errors(
              "syntax = \"proto2\";\nmessage M { extensions 10 to 20; optional int32 x = 15; }") ==
          diagnostics_t{ "2:53: x's number, 15, is in an extension range" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { reserved 5 to 2; }") ==
          diagnostics_t{ "2:22: a range ends at a number no lower than where it starts" });
}

TEST_CASE("an enum's values", "[proto]") {
    CHECK(parse_errors("syntax = \"proto3\";\nenum E { A = 0; B = 0; }") ==
          diagnostics_t{ "2:17: B has A's number, 0; values share one only with option "
                         "allow_alias = true" });
    CHECK(parse_errors("syntax = \"proto3\";\nenum E { option allow_alias = true; A = 0; B = 0; }")
              .empty());
    CHECK(parse_errors("syntax = \"proto3\";\nenum E { option allow_alias = true; A = 0; }") ==
          diagnostics_t{ "2:6: E sets allow_alias, but no two of its values share a number" });
    CHECK(parse_errors("syntax = \"proto3\";\nenum E {}") ==
          diagnostics_t{ "2:6: an enum needs a value" });
    CHECK(parse_errors("syntax = \"proto2\";\nenum E { A = -1; B = 2147483648; }") ==
          diagnostics_t{ "2:22: an enum's value is an int32, which 2147483648 isn't" });
    CHECK(parse_errors("syntax = \"proto2\";\nenum E { reserved 1, -5 to -3; A = -4; }") ==
          diagnostics_t{ "2:32: A's number, -4, is reserved" });
}

TEST_CASE("a map's key and a oneof's fields", "[proto]") {
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { map<double, int32> m = 1; }") ==
          diagnostics_t{ "2:17: a map's key is an integer, a bool, or a string, not double" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { map<M, int32> m = 1; }") ==
          diagnostics_t{ "2:17: a map's key is an integer, a bool, or a string, not M" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { repeated map<int32, int32> m = 1; }") ==
          diagnostics_t{ "2:13: a map field takes no label: it's repeated already" });
    CHECK(
        parse_errors("syntax = \"proto3\";\nmessage M { oneof o { map<int32, int32> m = 1; } }") ==
        diagnostics_t{ "2:23: a oneof can't have a map field" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { oneof o { optional int32 x = 1; } }") ==
          diagnostics_t{ "2:23: a oneof's fields take no label: they're each optional" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { oneof o {} }") ==
          diagnostics_t{ "2:19: a oneof needs a field" });
}

TEST_CASE("a declaration's options", "[proto]") {
    CHECK(parse_errors(
              "syntax = \"proto3\";\noption go_package = \"x\";\noption (my.opt).a = { b: 1 };")
              .empty());
    CHECK(parse_errors("syntax = \"proto3\";\noption java_pakage = \"x\";") ==
          diagnostics_t{ "2:8: 'java_pakage' isn't an option of a file" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 1 [packd = true]; }") ==
          diagnostics_t{ "2:26: 'packd' isn't an option of a field" });
    CHECK(parse_errors("syntax = \"proto3\";\nenum E { option deprecated = 1; A = 0; }") ==
          diagnostics_t{ "2:30: deprecated is true or false" });
    CHECK(parse_errors("syntax = \"proto3\";\noption features.field_presence = EXPLICIT;") ==
          diagnostics_t{ "2:8: features are an edition's, and a .proto file here is proto2 or "
                         "proto3" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 1 [packed = true]; }") ==
          diagnostics_t{ "2:26: only a repeated field of numbers, bools, or enums is packed" });
    CHECK(parse_errors(
              "syntax = \"proto3\";\nmessage M { repeated string x = 1 [packed = true]; }") ==
          diagnostics_t{ "2:36: only a repeated field of numbers, bools, or enums is packed" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { int32 x = 1 [json_name = y]; }") ==
          diagnostics_t{ "2:38: json_name is a string" });
    CHECK(parse_errors("syntax = \"proto3\";\nmessage M { option map_entry = true; }") ==
          diagnostics_t{ "2:20: map_entry isn't set by hand; a map field is map<K, V>" });
    CHECK(parse_errors(
              "syntax = \"proto2\";\nmessage M { option message_set_wire_format = true; }") ==
          diagnostics_t{ "2:20: a message set's wire format isn't supported" });
}

TEST_CASE("a type is found by protobuf's scoping rules", "[proto]") {
    // The scope nearest the name first, then each around it, and a leading '.' from the top.
    CHECK(resolve_errors(R"(syntax = "proto3";
package a.b;
message Outer {
  message Inner { Inner self = 1; Outer up = 2; .a.b.Outer top = 3; b.Outer pkg = 4; }
  Inner inner = 1;
  Outer.Inner qualified = 2;
  Sibling.Deep deep = 3;
}
message Sibling { message Deep {} }
)")
              .empty());

    // An import's types, and those of the files it imports publicly, but not privately.
    const std::vector<std::pair<std::string, std::string>> files{
        {
            "main.proto",
            "syntax = \"proto3\";\nimport \"a.proto\";\nmessage M {\n  pa.A a = 1;\n"
            "  pb.B b = 2;\n  pc.C c = 3;\n}\n",
        },
        {
            "a.proto",
            "syntax = \"proto3\";\npackage pa;\nimport public \"b.proto\";\nimport "
            "\"c.proto\";\nmessage A {}\n",
        },
        { "b.proto", "syntax = \"proto3\";\npackage pb;\nmessage B {}\n" },
        { "c.proto", "syntax = \"proto3\";\npackage pc;\nmessage C {}\n" },
    };
    CHECK(resolve(files) ==
          diagnostics_t{ "main.proto:6:3: 'pc.C' isn't a message or an enum that this file "
                         "declares or imports" });
}

TEST_CASE("a name that isn't a type a field or an rpc can take", "[proto]") {
    CHECK(resolve_errors("syntax = \"proto3\";\nmessage M { Missing x = 1; }") ==
          diagnostics_t{ "test.proto:2:13: 'Missing' isn't a message or an enum that this file "
                         "declares or imports" });
    // The nearest x that's a message or package is the one whose N it looks for; a field named
    // x is passed over.
    CHECK(resolve_errors("syntax = \"proto3\";\nmessage M {\n  message x {}\n  x.N y = 1;\n}\n"
                         "message x { message N {} }") ==
          diagnostics_t{ "test.proto:4:3: 'x.N' is M.x.N here, which isn't declared; the scope "
                         "nearest it is looked in first, and a '.' before a name, as in '.x.N', "
                         "starts from the outermost" });
    CHECK(resolve_errors("syntax = \"proto3\";\nmessage M {\n  int32 x = 1;\n  x.N y = 2;\n}\n"
                         "message x { message N {} }")
              .empty());
    CHECK(resolve_errors("syntax = \"proto3\";\nmessage M { int32 x = 1; .M.x y = 2; }") ==
          diagnostics_t{ "test.proto:2:26: '.M.x' is a field, not a message or an enum" });
    CHECK(resolve_errors("syntax = \"proto3\";\nenum E { A = 0; }\nmessage M {}\nservice S {\n"
                         "  rpc F(E) returns (M);\n  rpc G(M) returns (string);\n}") ==
          diagnostics_t{
              "test.proto:5:9: an rpc takes and gives messages, not the enum E",
              "test.proto:6:21: an rpc takes and gives messages, not string",
          });
}

TEST_CASE("a .proto declares each name once", "[proto]") {
    CHECK(resolve_errors("syntax = \"proto3\";\nmessage M { int32 x = 1; message x {} }") ==
          diagnostics_t{ "test.proto:2:34: 'x' is declared already in M, as a field" });
    const diagnostics_t values =
        resolve_errors("syntax = \"proto3\";\nenum A { NONE = 0; }\nenum B { NONE = 0; }");
    CHECK(values == diagnostics_t{ "test.proto:3:10: 'NONE' is declared already in the file, as "
                                   "an enum's value" });
    CHECK(resolve({
              { "main.proto", "syntax = \"proto3\";\npackage p;\nimport \"o.proto\";\nmessage "
                              "Taken {}\n" },
              { "o.proto", "syntax = \"proto3\";\npackage p;\nmessage Taken {}\n" },
          }) == diagnostics_t{ "main.proto:4:9: 'Taken' is declared already in p, in \"o.proto\", "
                               "as a message" });
}

TEST_CASE("what a field's type decides", "[proto]") {
    CHECK(resolve({
              { "main.proto",
                "syntax = \"proto3\";\nimport \"old.proto\";\nmessage M { Old o = 1; }\n" },
              { "old.proto", "syntax = \"proto2\";\nenum Old { A = 1; }\n" },
          }) == diagnostics_t{ "main.proto:3:13: a proto3 message's enums are proto3's, which "
                               "keep numbers they don't name; Old is proto2's" });
    CHECK(resolve_errors(
              "syntax = \"proto2\";\nenum E { A = 1; }\nmessage M { map<int32, E> m = 1; }") ==
          diagnostics_t{ "test.proto:3:24: a map's enum values start at 0, which an entry without "
                         "one has; E's first value isn't 0" });
    CHECK(resolve_errors("syntax = \"proto2\";\nmessage M { repeated M m = 1 [packed = true]; }") ==
          diagnostics_t{ "test.proto:2:22: only a repeated field of numbers, bools, or enums is "
                         "packed, not messages" });
    CHECK(resolve_errors(R"(syntax = "proto2";
enum E { A = 1; B = 2; }
message M {
  optional bool b = 1 [default = 1];
  optional int32 small = 2 [default = 2147483648];
  optional int32 least = 3 [default = -2147483648];
  optional uint32 positive = 4 [default = -1];
  optional uint64 large = 5 [default = 18446744073709551615];
  optional string s = 6 [default = 5];
  optional string bad = 7 [default = "\377"];
  optional bytes raw = 8 [default = "\377"];
  optional E e = 9 [default = C];
  optional M m = 10 [default = 1];
  optional double d = 11 [default = nan];
  optional float f = 12 [default = x];
}
)") == diagnostics_t{
           "test.proto:4:34: b's default is true or false",
           "test.proto:5:39: 2147483648 doesn't fit in int32, small's type",
           "test.proto:7:43: positive is uint32, which has no negative numbers",
           "test.proto:9:36: s's default is a string, in quotes",
           "test.proto:10:38: bad's default is UTF-8, as every String is",
           "test.proto:12:31: e's default is one of E's values",
           "test.proto:13:32: m is a message, which has no default",
           "test.proto:15:36: f's default is a number",
       });
}

TEST_CASE("each declaration's FutureBlitz name is its own", "[proto]") {
    const parsed_t parsed = parse("syntax = \"proto3\";\nmessage Outer { message Inner {} }\n"
                                  "message End {}\nmessage String { enum Self { A = 0; } }\n");
    CHECK(parsed.diagnostics.empty());
    CHECK(fb::proto_message_name(parsed.file, 1) == "Outer_Inner");
    CHECK(fb::proto_message_name(parsed.file, 2) == "End_");
    CHECK(fb::proto_message_name(parsed.file, 3) == "String_");
    CHECK(fb::proto_enum_name(parsed.file, 0) == "String_Self");

    CHECK(resolve_errors("syntax = \"proto3\";\nmessage A { message B {} }\nmessage A_B {}") ==
          diagnostics_t{ "test.proto:3:9: A_B is A_B in FutureBlitz, which another of this "
                         "file's declarations is too; rename one" });
    CHECK(resolve_errors("syntax = \"proto3\";\nenum E { End = 0; End_ = 1; }") ==
          diagnostics_t{ "test.proto:2:19: End_ is End_ in FutureBlitz, as another of E's values "
                         "is" });
    CHECK(resolve_errors("syntax = \"proto2\";\nmessage M {\n  optional int32 x = 1 [default = "
                         "2];\n  optional int32 xOrDefault = 2;\n}") ==
          diagnostics_t{ "test.proto:3:18: x's default is xOrDefault in FutureBlitz, which "
                         "another of M's members is too; rename one" });
}

TEST_CASE("an Import of a .proto file gives the module fb makes from it", "[proto]") {
    const fb::compilation_t compilation = compile_with(
        "Import std.core\nImport \"protos/shapes.proto\"\n"
        "Local s = shapes.Square(side: 2)\nLocal t = Square()\nPrint(\"{s.side} {t.side}\")",
        {
            {
                .path = "protos/shapes.proto",
                .text = "syntax = \"proto3\";\npackage geo;\nmessage Square { int32 side = 1; }\n",
            },
        });
    CHECK(diagnostics_of(compilation).empty());
    const auto named = std::ranges::find(compilation.ast.modules().modules(), "protos.shapes.proto",
                                         &fb::module_t::name);
    CHECK(named != compilation.ast.modules().modules().end());
    const std::string module = generated(compilation, "protos/shapes.proto");
    CHECK(module.contains("Public Struct Square Implements"));
    CHECK(module.contains("Public Field side: Int32"));
}

TEST_CASE("a .proto's imports are found from its folder up, and protobuf's well-known types in "
          "std",
          "[proto]") {
    const fb::compilation_t compilation = compile_with(
        "Import \"protos/game/game.proto\"\nLocal g = Game()",
        {
            {
                .path = "protos/game/game.proto",
                .text = "syntax = \"proto3\";\nimport \"common/types.proto\";\n"
                        "import \"google/protobuf/timestamp.proto\";\n"
                        "message Game { common.Id id = 1; google.protobuf.Timestamp at = 2; }\n",
            },
            {
                .path = "protos/common/types.proto",
                .text = "syntax = \"proto3\";\npackage common;\nmessage Id { string value = 1; }\n",
            },
        });
    CHECK(diagnostics_of(compilation).empty());
    const std::string module = generated(compilation, "protos/game/game.proto");
    CHECK(module.contains("Import \"protos/common/types.proto\" As types_"));
    CHECK(
        module.contains("Import \"std/data/proto/google/protobuf/timestamp.proto\" As timestamp_"));
}

TEST_CASE("a .proto's imports that don't work", "[proto]") {
    CHECK(diagnostics_of(
              compile_with("Import \"a.proto\"",
                           { { .path = "a.proto",
                               .text = "syntax = \"proto3\";\nimport \"missing.proto\";\n" } })) ==
          diagnostics_t{ "a.proto:2:8: there's no \"missing.proto\" in this file's folder or the "
                         "folders above it, up to the main file's folder" });
    CHECK(diagnostics_of(compile_with(
              "Import \"sub/a.proto\"",
              { { .path = "sub/a.proto", .text = "syntax = \"proto3\";\nimport \"../b.proto\";\n" },
                { .path = "b.proto", .text = "syntax = \"proto3\";\n" } })) ==
          diagnostics_t{ "sub/a.proto:2:8: an import's path goes down from a folder, with '/' "
                         "between folders and without '.' or '..', like \"shapes/circle.proto\"" });
    CHECK(diagnostics_of(compile_with(
              "Import \"a.proto\"",
              { { .path = "a.proto", .text = "syntax = \"proto3\";\nimport \"b.proto\";\n" },
                { .path = "b.proto", .text = "syntax = \"proto3\";\nimport \"a.proto\";\n" } })) ==
          diagnostics_t{ "b.proto:2:8: .proto files can't import each other in a cycle: a.proto "
                         "-> b.proto -> a.proto" });
    CHECK(diagnostics_of(compile_with("Import \"none.proto\"", {})) ==
          diagnostics_t{ "main.fb:1:8: there's no module \"none.proto\": no none.proto" });
}

TEST_CASE("a .proto with errors gives an empty module, past them", "[proto]") {
    const fb::compilation_t compilation = compile_with(
        "Import \"bad.proto\"\nLocal m = M()",
        { { .path = "bad.proto", .text = "syntax = \"proto3\";\nmessage M { Gone g = 1; }\n" } });
    CHECK(diagnostics_of(compilation) ==
          diagnostics_t{
              "main.fb:2:11: 'M' isn't declared",
              "bad.proto:2:13: 'Gone' isn't a message or an enum that this file declares or "
              "imports",
          });
    CHECK(generated(compilation, "bad.proto") ==
          "// bad.proto has errors, so fb makes nothing from it.\n");

    // Its module still imports the files it does, which are in the program only that way, so
    // nothing else of them is an error.
    const fb::compilation_t importing = compile_with(
        "Import \"bad.proto\"",
        {
            {
                .path = "bad.proto",
                .text = "syntax = \"proto3\";\nimport \"ok.proto\";\nmessage Bad { int32 x = ; }\n",
            },
            {
                .path = "ok.proto",
                .text = "syntax = \"proto3\";\nmessage Ok { map<int32, string> m = 1; }\n",
            },
        });
    CHECK(diagnostics_of(importing) ==
          diagnostics_t{ "bad.proto:3:25: expected the field's number, found ';'" });
    CHECK(generated(importing, "bad.proto") == "// bad.proto has errors, so fb makes nothing from "
                                               "it.\nImport \"ok.proto\" As Imported0_\n");
}

TEST_CASE("the module of a .proto imports each file it does, whose types it names or not",
          "[proto]") {
    const fb::compilation_t compilation = compile_with(
        "Import \"main.proto\"\nLocal m = Main()",
        {
            {
                .path = "main.proto",
                .text =
                    "syntax = \"proto3\";\nimport \"unused.proto\";\nimport \"forward.proto\";\n"
                    "import \"google/protobuf/descriptor.proto\";\n"
                    "extend google.protobuf.FieldOptions { string label = 50000; }\n"
                    "message Main { New n = 1 [(label) = \"n\"]; }\n",
            },
            {
                .path = "unused.proto",
                .text = "syntax = \"proto3\";\nmessage Unused { map<int32, string> m = 1; }\n",
            },
            {
                .path = "forward.proto",
                .text = "syntax = \"proto3\";\nimport public \"new.proto\";\n",
            },
            {
                .path = "new.proto",
                .text = "syntax = \"proto3\";\nmessage New { int32 v = 1; }\n",
            },
        });
    CHECK(diagnostics_of(compilation).empty());
    const std::string module = generated(compilation, "main.proto");
    CHECK(module.contains(
        "Import \"forward.proto\" As forward_\nImport \"new.proto\" As new_\n"
        "Import \"std/data/proto/google/protobuf/descriptor.proto\" As descriptor_\n"
        "Import \"unused.proto\" As unused_\n"));
    // A program that names a type of a .proto it doesn't import is told the Import, by the
    // .proto's path.
    CHECK(diagnostics_of(
              compile_with("Import \"main.proto\"\nLocal f = Fresh()",
                           {
                               {
                                   .path = "main.proto",
                                   .text = "syntax = \"proto3\";\nimport \"new.proto\";\nmessage "
                                           "Main { Fresh f = 1; }\n",
                               },
                               {
                                   .path = "new.proto",
                                   .text = "syntax = \"proto3\";\nmessage Fresh { int32 v = 1; }\n",
                               },
                           })) ==
          diagnostics_t{ "main.fb:2:11: 'Fresh' is in new.proto, which this file doesn't import: "
                         "Import \"new.proto\"" });
}

TEST_CASE("the module of a .proto has a Struct for each message, and an Enum for each enum",
          "[proto]") {
    constexpr std::string_view tree = R"(syntax = "proto3";
message Node {
  enum Kind { LEAF = 0; BRANCH = 1; }
  Kind kind = 1;
  Node left = 2;
  repeated Node children = 3;
  oneof label { string name = 4; int32 id = 5; }
}
)";
    const fb::compilation_t compilation =
        compile_with("Import \"tree.proto\"\nLocal n = Node()",
                     { { .path = "tree.proto", .text = std::string(tree) } });
    CHECK(diagnostics_of(compilation).empty());
    const std::string module = generated(compilation, "tree.proto");
    // proto3's enums are @Open, and a Node holds a Node in a ProtoBox, behind a Property.
    CHECK(module.contains("@Open Public Enum Node_Kind: Int32"));
    CHECK(module.contains("Field left_: Proto_.ProtoBox<Node>?"));
    CHECK(module.contains("Public Property left: Node?"));
    CHECK(module.contains("Public Field children: Node[]"));
    CHECK(module.contains("Public Enum Node_label: Int32"));
    CHECK(module.contains("Public Property label: Node_label"));
}
