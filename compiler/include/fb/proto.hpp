#pragma once

#include "fb/diagnostic.hpp"
#include "fb/source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

struct proto_file_t;

/// A .proto file's syntax, as its syntax line says: proto2 without one.
enum class proto_syntax_t : std::uint8_t {
    PROTO2,
    PROTO3,
};

/// A field's label.
enum class proto_label_t : std::uint8_t {
    /// None written: a proto3 field, which has no presence, so its zero isn't written, or a
    /// oneof's field or a map, which take none.
    NONE,
    OPTIONAL,
    REQUIRED,
    REPEATED,
};

/// What a field holds: one of protobuf's scalar types, or a message or an enum, which its type
/// names.
enum class proto_kind_t : std::uint8_t {
    DOUBLE,
    FLOAT,
    INT32,
    INT64,
    UINT32,
    UINT64,
    SINT32,
    SINT64,
    FIXED32,
    FIXED64,
    SFIXED32,
    SFIXED64,
    BOOL,
    STRING,
    BYTES,
    /// A message or an enum by name, until resolve_proto() finds which.
    NAMED,
    MESSAGE,
    ENUM,
};

/// A type a field or an rpc names, and what resolve_proto() finds it is.
struct proto_type_t {
    proto_kind_t kind = proto_kind_t::NAMED;
    /// A message's or enum's name as written, like "Outer.Inner" or ".pkg.Inner". Empty for a
    /// scalar.
    std::string name;
    span_t span;
    /// The file that declares the message or enum it names, once it's resolved.
    const proto_file_t* file = nullptr;
    /// Its index in that file's messages or enums.
    std::size_t index = 0;
};

/// What a constant in an option or a default is.
enum class proto_constant_kind_t : std::uint8_t {
    /// A name, like true, inf, or an enum's value: `text`.
    IDENTIFIER,
    /// An integer: `integer`, and `negative` for a '-'.
    INTEGER,
    /// A number with a fraction or an exponent, or inf or nan after a sign: `number`.
    FLOAT,
    /// Strings in quotes, each after the one before: `text`, their bytes, escapes undone.
    STRING,
    /// A message's fields in braces, for an option of a message type, which isn't read.
    AGGREGATE,
};

/// A constant an option or a default gives, as written.
struct proto_constant_t {
    proto_constant_kind_t kind = proto_constant_kind_t::IDENTIFIER;
    std::string text;
    std::uint64_t integer = 0;
    bool negative = false;
    double number = 0;
    /// An integer past 64 bits, whose `integer` is the most 64 bits hold, and whose `number` is
    /// it as a double when it's decimal.
    bool too_big = false;
    span_t span;
};

/// A field of a message.
struct proto_field_t {
    std::string name;
    span_t name_span;
    std::int32_t number = 0;
    span_t number_span;
    proto_label_t label = proto_label_t::NONE;
    proto_type_t type;
    /// A map, map<K, V>: K, and `type` is V.
    std::optional<proto_type_t> key;
    /// Its oneof, by index in its message's oneofs, when it's in one.
    std::optional<std::size_t> oneof;
    /// [default = ...], proto2's.
    std::optional<proto_constant_t> default_value;
    /// [packed = ...], when it's written.
    std::optional<bool> packed;
    bool deprecated = false;
    /// A proto2 group, whose braces declare the message `type` names: its fields go between a
    /// start tag and an end tag.
    bool group = false;
};

/// A message's oneof: at most one of its fields holds a value.
struct proto_oneof_t {
    std::string name;
    span_t name_span;
};

/// Field numbers from `low` to `high`, both in it.
struct proto_range_t {
    std::int32_t low = 0;
    std::int32_t high = 0;
    span_t span;
};

struct proto_message_t {
    std::string name;
    span_t name_span;
    /// Its package's name and the messages it's in, then its own, as protobuf names it:
    /// "pkg.Outer.Inner".
    std::string full_name;
    /// The message it's declared in, by index in its file's messages.
    std::optional<std::size_t> parent;
    std::vector<proto_field_t> fields;
    std::vector<proto_oneof_t> oneofs;
    std::vector<proto_range_t> reserved;
    std::vector<std::string> reserved_names;
    std::vector<proto_range_t> extensions;
    bool deprecated = false;
    /// The message a group's braces declare.
    bool group = false;
};

struct proto_enum_value_t {
    std::string name;
    span_t name_span;
    std::int32_t number = 0;
};

struct proto_enum_t {
    std::string name;
    span_t name_span;
    /// "pkg.Outer.Color".
    std::string full_name;
    /// The message it's declared in, by index in its file's messages.
    std::optional<std::size_t> parent;
    std::vector<proto_enum_value_t> values;
    std::vector<proto_range_t> reserved;
    std::vector<std::string> reserved_names;
    /// option allow_alias = true: values can share a number.
    bool allow_alias = false;
    bool deprecated = false;
};

struct proto_method_t {
    std::string name;
    span_t name_span;
    proto_type_t input;
    proto_type_t output;
    /// `stream` before the request or the response.
    bool client_streaming = false;
    bool server_streaming = false;
    bool deprecated = false;
};

struct proto_service_t {
    std::string name;
    span_t name_span;
    /// "pkg.Greeter".
    std::string full_name;
    std::vector<proto_method_t> methods;
    bool deprecated = false;
};

/// An import line: another .proto file, whose types this one can name.
struct proto_import_t {
    /// As written, like "google/protobuf/timestamp.proto".
    std::string path;
    span_t span;
    /// `import public`: what imports this file can name the other's types too.
    bool is_public = false;
    /// The file it names, once the driver has found it. Null when it isn't there.
    const proto_file_t* file = nullptr;
};

/// A .proto file, read.
struct proto_file_t {
    file_id_t file{};
    /// Its path in the program, like "proto/game.proto", or "std/data/proto/google/protobuf/
    /// any.proto" for one of the well-known types std has.
    std::string path;
    proto_syntax_t syntax = proto_syntax_t::PROTO2;
    /// Its package, like "game.net", or empty.
    std::string package;
    std::vector<proto_import_t> imports;
    /// Every message, the nested ones too, each after the one it's in.
    std::vector<proto_message_t> messages;
    /// Every enum, the nested ones too.
    std::vector<proto_enum_t> enums;
    std::vector<proto_service_t> services;
    /// Every identifier in it, for names the module it makes needs that none of them can be.
    std::vector<std::string> identifiers;
};

/// Reads a .proto file, proto2 or proto3: its syntax line, package, imports, options, messages
/// (with their fields, oneofs, maps, groups, nested types, and reserved and extension ranges),
/// enums, services, and extends, which it reads past. What's wrong is an error at its line and
/// column, and what has errors is left out.
[[nodiscard]] proto_file_t parse_proto(const source_file_t& file, diagnostic_bag_t& diagnostics);

/// Finds what `file`'s fields and rpcs name, by protobuf's scoping rules: among its own messages
/// and enums, and those of the files its imports name, and the files those import publicly.
/// Then checks what depends on the types: map keys, enums in proto3 messages, defaults, packed,
/// and that no two of its messages, enums, and services have one name in FutureBlitz. Each
/// import's `file` has to be set, or be null for one that wasn't found, which is reported
/// already.
void resolve_proto(proto_file_t& file, diagnostic_bag_t& diagnostics);

/// The FutureBlitz name of a message of `file`: its own, after those of the messages it's in,
/// with '_' between, like "Outer_Inner", with a '_' after one that's a keyword or a built-in
/// type's name.
[[nodiscard]] std::string proto_message_name(const proto_file_t& file, std::size_t index);

/// The FutureBlitz name of an enum of `file`, as proto_message_name() gives a message's.
[[nodiscard]] std::string proto_enum_name(const proto_file_t& file, std::size_t index);

/// The FutureBlitz module `file` gives, which resolve_proto() has resolved without errors: a
/// Struct for each message, an Enum for each enum (@Open for proto3's), and an Interface of
/// Async Methods for each service, with what reads and writes them in protobuf's wire format,
/// through std.data.protobuf. It imports the modules of the files it imports by their paths in
/// the program, which the driver resolves.
[[nodiscard]] std::string generate_proto(const proto_file_t& file);

/// The module of a .proto file with errors, shown as `shown`, which has nothing of it but the
/// Imports of the files it imports that the driver found, as their modules are in the program
/// only through an Import.
[[nodiscard]] std::string failed_proto_module(const proto_file_t& file, std::string_view shown);

} // namespace fb
