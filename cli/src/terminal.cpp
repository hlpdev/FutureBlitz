#include "fb/terminal.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
// shellapi.h needs windows.h first.
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace fb {

#ifdef _WIN32

namespace {

HANDLE handle(const stream_t stream) {
    return GetStdHandle(stream == stream_t::STANDARD_OUTPUT ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}

/// UTF-16 to UTF-8.
std::string narrow(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

} // namespace

// argv is in the ANSI code page, so read the UTF-16 command line instead.
std::vector<std::string> command_line(const int /*argc*/, char** /*argv*/) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return {};
    }

    std::vector<std::string> out;
    for (const wchar_t* argument :
         std::span(arguments, static_cast<std::size_t>(count)).subspan(1)) {
        out.push_back(narrow(argument));
    }

    LocalFree(static_cast<HLOCAL>(arguments));
    return out;
}

namespace {

/// What the console was before prepare_terminal() changed it: its output code page, 0 without
/// a console, and each stream's mode, where it's the console's. The console is the shell's
/// too, so it gets them back as the process exits.
struct console_before_t {
    UINT code_page = 0;
    std::array<std::optional<DWORD>, 2> modes;
};

console_before_t& console_before() {
    static console_before_t before;
    return before;
}

void restore_console() {
    const console_before_t& before = console_before();
    if (before.code_page != 0) {
        SetConsoleOutputCP(before.code_page);
    }

    for (const stream_t stream : { stream_t::STANDARD_OUTPUT, stream_t::STANDARD_ERROR }) {
        if (const std::optional<DWORD> mode = before.modes.at(std::to_underlying(stream))) {
            SetConsoleMode(handle(stream), *mode);
        }
    }
}

} // namespace

void prepare_terminal() {
    // Lines end in "\n" like the programs fb builds, not "\r\n".
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    console_before_t& before = console_before();
    before.code_page = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    for (const stream_t stream : { stream_t::STANDARD_OUTPUT, stream_t::STANDARD_ERROR }) {
        DWORD mode = 0;
        if (GetConsoleMode(handle(stream), &mode) != 0) {
            before.modes.at(std::to_underlying(stream)) = mode;
            SetConsoleMode(handle(stream), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }

    std::atexit(restore_console);
}

bool is_terminal(const stream_t stream) {
    // Only a console has a mode, so this fails for pipes and files.
    DWORD mode = 0;
    return GetConsoleMode(handle(stream), &mode) != 0;
}

#else

std::vector<std::string> command_line(const int argc, char** argv) {
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    return { arguments.begin() + 1, arguments.end() };
}

void prepare_terminal() {}

bool is_terminal(const stream_t stream) {
    return isatty(stream == stream_t::STANDARD_OUTPUT ? STDOUT_FILENO : STDERR_FILENO) != 0;
}

#endif

bool use_color(const color_t choice, const stream_t stream) {
    switch (choice) {
    case color_t::ALWAYS:
        return true;
    case color_t::NEVER:
        return false;
    case color_t::AUTO:
        break;
    }

    // https://no-color.org: any NO_COLOR that isn't empty turns color off.
    const char* no_color = std::getenv("NO_COLOR");
    if (no_color != nullptr && *no_color != '\0') {
        return false;
    }

    return is_terminal(stream);
}

} // namespace fb
