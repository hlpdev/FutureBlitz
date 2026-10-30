#pragma once

#include "fb/ast.hpp"
#include "fb/codegen.hpp"
#include "fb/diagnostic.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

struct compilation_t;

/// How fb test tests an example: a code block under a doc comment's "# Examples" heading.
enum class example_kind_t : std::uint8_t {
    /// "```" or "```fb": it compiles and runs.
    RUN,
    /// "```fb no_run": it only compiles, for code that would wait on the network or a person.
    NO_RUN,
};

/// One line of an example's code.
struct example_line_t {
    /// Its code, without the "# " that hides it.
    std::string text;
    /// Where that code is in the file.
    span_t span;
    /// Written after "# ": the test compiles it, but the docs don't show it.
    bool hidden = false;
};

/// A code block under a doc comment's "# Examples" heading, in FutureBlitz, which fb test runs.
struct doc_example_t {
    example_kind_t kind = example_kind_t::RUN;
    /// Its lines, hidden ones too.
    std::vector<example_line_t> lines;
    /// Its opening fence's line in the file.
    span_t fence;

    /// Its code as the docs show it: the lines that aren't hidden.
    [[nodiscard]] std::string shown() const;
};

/// A "[Name]" link in a doc comment: what it names, which fb doc finds as code where the comment
/// is would, its declaration's members first.
struct doc_link_t {
    /// What it names, as written between its brackets, without backticks, a "()" after it, or
    /// type arguments, like "Stack.Push" for "[`Stack<T>.Push()`]".
    std::string target;
    /// Where it is in the file, brackets and all.
    span_t span;
};

/// The part of a doc comment under one heading.
struct doc_section_t {
    /// Its heading's text, like "Errors". Empty for what comes before the first heading.
    std::string heading;
    /// Its Markdown, without its examples' hidden lines.
    std::string markdown;
};

/// A doc comment's Markdown, read: Rust's way, its first sentence is its summary, headings like
/// "# Errors", "# Panics", and "# Examples" start its sections, and "[Name]" links name
/// declarations.
struct doc_t {
    /// Its first sentence: its first paragraph up to a '.', '!', or '?' that's followed by a space
    /// or ends it, outside code, or the whole paragraph without one, on one line. Empty when the
    /// comment starts with a heading or a code block.
    std::string summary;
    /// Its Markdown, without its examples' hidden lines.
    std::string markdown;
    std::vector<doc_section_t> sections;
    std::vector<doc_link_t> links;
    std::vector<doc_example_t> examples;
};

/// Reads a doc comment's Markdown.
[[nodiscard]] doc_t read_doc(const doc_comment_t& comment);

/// A "[Name]" link, found: the path of the declaration it names, as fb doc's JSON names
/// declarations, or a built-in's name.
struct doc_target_t {
    std::string text;
    std::string path;
    bool builtin = false;
};

/// One declaration of a package's public API, as fb doc writes it.
struct doc_item_t {
    /// What it is: "function", "struct", "class", "interface", "attribute", "enum", "alias",
    /// "global", "const", "field", "static field", "method", "static method", "constructor",
    /// "operator", "property", "enum member", or "extend".
    std::string kind;
    /// Its name: an Operator's is its operator, like "+", and a New's is "New". An Extend's is
    /// its type, as written without its type arguments, like "String" or "T[]".
    std::string name;
    /// Its module's name, its type's for a member, and its name:
    /// "std.collections.stack.Stack.Push". An Operator's ends with "Operator" and its operator,
    /// like "...Vec3.Operator+". Overloads share one. An Extend's is its module's, "Extend", and
    /// its name, like "std.text.regex.Extend.String", and its members' are its module's, its name,
    /// and theirs, like "std.text.regex.String.Matches".
    std::string path;
    /// Its first line as written, with its visibility and modifiers but without its attributes or
    /// comments, and one space for a line break: a function's to its result and Throws, a type's
    /// to its Implements, a variable's to its type, or with its value when it's a Const or has no
    /// type.
    std::string signature;
    /// The attributes before it, as written, like "@PanicsAtCaller" or "@Deprecated(\"...\")".
    std::vector<std::string> attributes;
    visibility_t visibility = visibility_t::PUBLIC;
    /// Its file, by its path from its package's folder, std's from std/, and its first line's
    /// number, from 1.
    std::string file;
    std::uint32_t line = 0;
    /// Where its name is, which fb doc --check points to, or an Extend's whole block.
    span_t span;
    std::optional<doc_t> doc;
    /// For a member without a doc comment of its own that implements an Interface's member or
    /// overrides a base Class's: the path of the one whose doc it has.
    std::string doc_from;
    /// What each of its doc's links names, in their order. One that names nothing has an empty
    /// path, which fb doc warned about.
    std::vector<doc_target_t> links;
    /// A type's members, an Enum's members, or an Extend's, in source order: those its users
    /// see, a Class's Protected ones among them.
    std::vector<doc_item_t> members;
    /// An Extend's type: the path of the declaration it extends, or a built-in type's name, like
    /// "String" or "T[]".
    std::string extends;
};

/// A module of a package, as fb doc writes it.
struct doc_module_t {
    std::string name;
    /// By its path from its package's folder.
    std::string file;
    /// Its file's start, which fb doc --check points to.
    span_t span;
    std::optional<doc_t> doc;
    std::vector<doc_target_t> links;
    /// Its Public declarations, and its Extends of what its users see, in source order.
    std::vector<doc_item_t> items;
    /// From a prebuilt library's interface file, whose examples' lines are its source's, as its
    /// "// line" comments say.
    bool prebuilt = false;
};

/// A package's public API, as fb doc writes it.
struct doc_package_t {
    std::string name;
    std::string version;
    /// Sorted by name. A module with no Public declarations and no doc comment is left out.
    std::vector<doc_module_t> modules;
};

/// The public API of the package that `compilation`'s main file is in, or with `standard`, the
/// standard library's, from a compilation that has every module of it. Its declarations' docs'
/// links are found as code where they are would find their names, and each that names nothing,
/// and each doc comment that's on no declaration, is a warning in `diagnostics`. `mode` is the
/// build the compilation was for, whose #If branches it has.
[[nodiscard]] doc_package_t document(const compilation_t& compilation, bool standard,
                                     build_mode_t mode, diagnostic_bag_t& diagnostics);

/// fb doc's JSON of a package's API, in the format docs/tooling.md describes. `sources` has the
/// files its docs are in.
[[nodiscard]] std::string doc_json(const doc_package_t& package, const source_manager_t& sources);

/// What fb doc --check finds in `package`, as errors in `diagnostics`, in its order: a module
/// without a //! doc comment, and an item but an Extend without a /// doc comment of its own,
/// even one that has the doc of the Interface member it implements or the Class member it
/// overrides, or one whose doc has no summary.
void check_docs(const doc_package_t& package, diagnostic_bag_t& diagnostics);

/// One of a file's examples, as a module that fb test compiles and runs: its text, and where
/// each of its lines is in the file, which its messages and stack traces show.
struct example_module_t {
    /// The test's name: the declaration's path in its module, like "game.shapes.Circle.Area",
    /// and "(example)", or "(example 2)" for its second, and so on.
    std::string name;
    std::string text;
    /// For each of its lines, where its first column is in the file.
    std::vector<location_t> lines;
    /// For each of its lines, whether fb added it, so all of it shows as at its fence.
    std::vector<bool> added;
    /// Its opening fence, in the file.
    span_t fence;
};

/// The examples of the declarations of a file, the module `module`, which `ast` has parsed with
/// its doc comments, as modules of their own, each a @Test Function of its code, which Imports
/// std.core, std.test, and the module, as `import` names it, unless it Imports them itself.
/// An example's Import and From lines go first, and its declarations, like a Struct for an
/// Interface's, go outside the test.
[[nodiscard]] std::vector<example_module_t> example_modules(const ast_t& ast,
                                                            const source_file_t& file,
                                                            std::string_view module,
                                                            std::string_view import);

} // namespace fb
