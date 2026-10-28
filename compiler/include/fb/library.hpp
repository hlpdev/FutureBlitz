#pragma once

#include <string>
#include <vector>

namespace fb {

/// The build an interface file's first line names, "// fbi physics 0.2.0 <hash>": the library's
/// name and version, and its ABI hash.
struct interface_header_t {
    std::string name;
    std::string version;
    /// 32 hex digits.
    std::string hash;
};

/// A prebuilt library's binary, its .fblib: the build it is, which its interface file's first
/// line has to name, the bundled C libraries its code calls, which a program that links it links,
/// by their names, and its code, LLVM bitcode.
struct library_binary_t {
    interface_header_t header;
    std::vector<std::string> bundled;
    std::string code;
};

} // namespace fb
