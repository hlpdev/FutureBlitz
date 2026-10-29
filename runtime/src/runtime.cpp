#include "fb/runtime.hpp"

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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
#include <malloc.h>
#include <windows.h>
#else
#include <csignal>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {

/// splitmix64's finalizer: each bit of `value` changes about half the bits of the result.
std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

/// Lets tests and tools tell a panic from a normal exit.
constexpr int panic_exit_code = 101;
/// A program that let an error out failed, like a command that couldn't do its job.
constexpr int error_exit_code = 1;
/// How many Causes an uncaught error's report shows after it.
constexpr std::size_t max_causes = 100;

/// An error's fields from Error, which every error's Class extends, so they come first.
fb_error_fields_t& error_fields(fb_object_t* error) {
    return *reinterpret_cast<fb_error_fields_t*>(error + 1);
}

const fb_error_fields_t& error_fields(const fb_object_t* error) {
    return *reinterpret_cast<const fb_error_fields_t*>(error + 1);
}

/// Keeps the low `bits` bits of `value`, sign-extending them when `is_signed`.
std::uint64_t truncate(const std::uint64_t value, const std::uint32_t bits, const bool is_signed) {
    if (bits == 64) {
        return value;
    }

    const std::uint64_t low = value & ((std::uint64_t{ 1 } << bits) - 1);
    if (!is_signed) {
        return low;
    }

    // Flipping the sign bit and then subtracting it copies the sign into the high bits.
    const std::uint64_t sign = std::uint64_t{ 1 } << (bits - 1);
    return (low ^ sign) - sign;
}

/// `base` to the power `exponent` modulo 2^64, by squaring. The low bits are right for signed
/// bases too.
std::uint64_t wrapping_power(std::uint64_t base, std::uint64_t exponent) {
    std::uint64_t result = 1;
    while (exponent > 0) {
        if ((exponent & 1U) != 0) {
            result *= base;
        }

        exponent >>= 1U;
        base *= base;
    }

    return result;
}

/// Sets `result` to `base` to the power `exponent`, by squaring, and returns true when that
/// doesn't fit in `Int`.
template <typename Int>
bool power_overflows(Int base, std::uint64_t exponent, Int& result) {
    result = 1;
    while (exponent > 0) {
        if ((exponent & 1U) != 0 && __builtin_mul_overflow(result, base, &result)) {
            return true;
        }

        // The last square is never used, so it mustn't count as an overflow.
        exponent >>= 1U;
        if (exponent > 0 && __builtin_mul_overflow(base, base, &base)) {
            return true;
        }
    }

    return false;
}

#ifdef _WIN32
/// What the console was before fb_start() changed it: its output code page, 0 without a
/// console, and stderr's mode, where it's the console's.
struct console_before_t {
    UINT code_page = 0;
    std::optional<DWORD> error_mode;
};

console_before_t& console_before() {
    static console_before_t before;
    return before;
}

/// Held while fb_start() changes the console, which console_ended() waits for, so the console
/// is never put back before it's done changing.
std::mutex& console_mutex() {
    static std::mutex mutex;
    return mutex;
}

void restore_console() {
    const console_before_t& before = console_before();
    if (before.code_page != 0) {
        SetConsoleOutputCP(before.code_page);
    }

    if (before.error_mode) {
        SetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), *before.error_mode);
    }
}

/// The console ends a program on Ctrl+C or Ctrl+Break, or when its window closes, without its
/// end or a panic's: what it changed that others share is put back first, once fb_start() has
/// changed it, and FALSE lets the default handler end it, as it would have.
BOOL WINAPI console_ended(const DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        const std::scoped_lock held(console_mutex());
        fb::runtime::run_end();
    }

    return FALSE;
}
#else
/// A signal that ends a program at its default, like Ctrl+C's SIGINT, ends it without its end or
/// a panic's: what it changed that others share is put back first, and the signal, at its
/// default again, ends it as it would have. What at_end() runs only calls what a signal handler
/// can, like fcntl.
void signal_ended(const int number) {
    fb::runtime::run_end();
    std::signal(number, SIG_DFL);
    std::raise(number);
}
#endif

/// Held while a line goes to stdout, so lines from different threads don't mix.
std::mutex& output() {
    static std::mutex instance;
    return instance;
}

/// Held by the first thread that ends the program, so another that panics meanwhile waits.
std::mutex& ending() {
    static std::mutex instance;
    return instance;
}

/// What at_end() was given. Any thread may end the program while another gives one.
std::array<std::atomic<void (*)()>, 8>& restorers() {
    static std::array<std::atomic<void (*)()>, 8> instance{};
    return instance;
}

/// Ends the program with `code` at once, with what it printed flushed, and what it changed that
/// others share put back. Exiting normally would destroy what other threads may still be using,
/// like the pool.
[[noreturn]] void end_program(const int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    fb::runtime::run_end();
    std::_Exit(code);
}

} // namespace

std::optional<std::string> fb::runtime::environment(const char* name) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::strlen(name));
    const wchar_t* value = _wgetenv(wide_name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }

    const int wide = static_cast<int>(std::wcslen(value));
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, wide, nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, wide, text.data(), size, nullptr, nullptr);
    return text;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }

    return std::string(value);
#endif
}

bool fb::runtime::stderr_colors() {
    const std::optional<std::string> no_color = environment("NO_COLOR");
    if (no_color && !no_color->empty()) {
        return false;
    }

#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0 &&
           (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

namespace {

/// Ends the program with "panic: <message> at <file>:<line>:<column>" on stderr, the message
/// whole, NULs too.
[[noreturn]] void panic_with(const std::string_view message, const fb_location_t* location) {
    // The lock is never let go: the program ends with it held.
    ending().lock();
    // Whatever the program printed shows up before the panic, without a line cut in half.
    output().lock();
    std::fflush(stdout);
    fb::runtime::run_before_report();
    const char* label = fb::runtime::stderr_colors() ? "\x1b[1;31mpanic\x1b[0m" : "panic";
    std::fprintf(stderr, "%s: ", label);
    std::fwrite(message.data(), 1, message.size(), stderr);
    if (location != nullptr) {
        std::fprintf(stderr, " at %s:%u:%u", location->file, location->line, location->column);
    }

    std::fputc('\n', stderr);
    end_program(panic_exit_code);
}

} // namespace

namespace fb::runtime {

void write_string(const fb_string_t* string, std::FILE* stream) {
    std::fwrite(string + 1, 1, static_cast<std::size_t>(string->length), stream);
}

void write_prompt(const fb_string_t* prompt) {
    const std::scoped_lock lock(output());
    write_string(prompt, stdout);
    std::fflush(stdout);
    std::fflush(stderr);
}

namespace {

/// What at_finish() was given.
std::array<void (*)(), 8>& finishers() {
    static std::array<void (*)(), 8> instance{};
    return instance;
}

/// What at_code_end() was given, which any thread can add to.
std::array<std::atomic<void (*)()>, 8>& code_enders() {
    static std::array<std::atomic<void (*)()>, 8> instance{};
    return instance;
}

/// Whether run_code_end() has run, after which at_code_end() runs what it's given at once.
std::atomic<bool>& code_ended() {
    static std::atomic<bool> instance = false;
    return instance;
}

} // namespace

void at_code_end(void (*done)()) {
    for (std::atomic<void (*)()>& slot : code_enders()) {
        void (*expected)() = nullptr;
        if (slot.compare_exchange_strong(expected, done) || expected == done) {
            break;
        }
    }

    // run_code_end() marks the code done before it looks at the slots, and this looks at the mark
    // after it fills one, both in one order for every thread, so one of them sees the other.
    if (code_ended().load()) {
        done();
    }
}

void run_code_end() {
    code_ended().store(true);
    for (const std::atomic<void (*)()>& slot : std::views::reverse(code_enders())) {
        if (void (*const done)() = slot.load(); done != nullptr) {
            done();
        }
    }
}

void at_finish(void (*stop)()) {
    for (void (*&slot)() : finishers()) {
        if (slot == nullptr || slot == stop) {
            slot = stop;
            return;
        }
    }
}

void run_finish() {
    for (void (*const stop)() : std::views::reverse(finishers())) {
        if (stop != nullptr) {
            stop();
        }
    }
}

void run_end() {
    for (const std::atomic<void (*)()>& slot : std::views::reverse(restorers())) {
        if (void (*const restore)() = slot.load(std::memory_order_acquire); restore != nullptr) {
            restore();
        }
    }
}

void at_end(void (*restore)()) {
    for (std::atomic<void (*)()>& slot : restorers()) {
        void (*expected)() = nullptr;
        if (slot.compare_exchange_strong(expected, restore, std::memory_order_acq_rel) ||
            expected == restore) {
            return;
        }
    }
}

namespace {

/// What before_report() was given.
std::atomic<void (*)()>& leaver() {
    static std::atomic<void (*)()> instance = nullptr;
    return instance;
}

} // namespace

void before_report(void (*leave)()) {
    leaver().store(leave, std::memory_order_release);
}

void run_before_report() {
    if (void (*const leave)() = leaver().load(std::memory_order_acquire); leave != nullptr) {
        leave();
    }
}

} // namespace fb::runtime

extern "C" {

void fb_start() {
#ifdef _WIN32
    // Strings are UTF-8, and binary mode keeps "\n" from becoming "\r\n".
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    // The console is the shell's too, so it gets its code page and stderr's mode back however
    // the program ends: what they were is kept, and what puts them back is in place, before
    // either changes, and a Ctrl+C or Ctrl+Break as the program starts waits for both changes.
    const std::scoped_lock held(console_mutex());
    console_before_t& before = console_before();
    before.code_page = GetConsoleOutputCP();
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(error, &mode) != 0) {
        before.error_mode = mode;
    }

    fb::runtime::at_end(restore_console);
    SetConsoleCtrlHandler(console_ended, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    // Lets panics print in color.
    if (before.error_mode) {
        SetConsoleMode(error, *before.error_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#else
    // Only a signal at its default: one the program was started ignoring stays ignored.
    for (const int number : { SIGINT, SIGTERM, SIGHUP }) {
        struct sigaction current{};
        if (sigaction(number, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
            struct sigaction ended{};
            ended.sa_handler = signal_ended;
            sigemptyset(&ended.sa_mask);
            sigaction(number, &ended, nullptr);
        }
    }
#endif
}

void* fb_mem_alloc(const std::uint64_t bytes) {
    return std::calloc(1, std::max<std::uint64_t>(bytes, 1));
}

void* fb_mem_alloc_uninitialized(const std::uint64_t bytes) {
    return std::malloc(std::max<std::uint64_t>(bytes, 1));
}

void* fb_mem_resize(void* memory, const std::uint64_t bytes) {
    return std::realloc(memory, std::max<std::uint64_t>(bytes, 1));
}

void fb_mem_free(void* memory) {
    std::free(memory);
}

void* fb_mem_alloc_aligned(const std::uint64_t bytes, const std::uint64_t alignment) {
    const std::uint64_t size = std::max<std::uint64_t>(bytes, 1);
#ifdef _WIN32
    void* memory = _aligned_malloc(size, alignment);
#else
    // posix_memalign takes an alignment of a pointer's at least.
    void* memory = nullptr;
    if (posix_memalign(&memory, std::max<std::uint64_t>(alignment, sizeof(void*)), size) != 0) {
        return nullptr;
    }
#endif
    if (memory != nullptr) {
        std::memset(memory, 0, size);
    }

    return memory;
}

void fb_mem_free_aligned(void* memory) {
#ifdef _WIN32
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

void fb_mem_copy(void* destination, const void* source, const std::uint64_t bytes) {
    std::memmove(destination, source, bytes);
}

void fb_mem_fill(void* destination, const std::uint8_t value, const std::uint64_t bytes) {
    std::memset(destination, value, bytes);
}

std::int32_t fb_mem_compare(const void* a, const void* b, const std::uint64_t bytes) {
    return std::memcmp(a, b, bytes);
}

void fb_write(const fb_array_t* texts, const std::int32_t to_error, const std::int32_t line) {
    const auto* const* strings = static_cast<const fb_string_t* const*>(texts->data);
    const std::scoped_lock lock(output());
    std::FILE* stream = to_error != 0 ? stderr : stdout;
    if (to_error != 0) {
        std::fflush(stdout);
    }

    for (std::int64_t i = 0; i < texts->count; ++i) {
        if (i > 0) {
            std::fputc(' ', stream);
        }

        fb::runtime::write_string(strings[i], stream);
    }

    if (line != 0) {
        std::fputc('\n', stream);
    }
}

void fb_flush() {
    const std::scoped_lock lock(output());
    std::fflush(stdout);
    std::fflush(stderr);
}

void fb_panic(const char* message, const fb_location_t* location) {
    panic_with(message, location);
}

void fb_load_beside(const char* executable, const char* file, const char* build,
                    const char* const* names, void** slots, const std::int64_t count) {
    const auto stop = [](const std::string& message) {
        std::fprintf(stderr, "error: %s\n", message.c_str());
        end_program(error_exit_code);
    };
#ifdef _WIN32
    std::wstring own(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));
        if (length < own.size()) {
            own.resize(length);
            break;
        }

        own.resize(own.size() * 2);
    }

    // Started through a link, or by its short name, it's its own file, by its long name.
    if (HANDLE handle =
            CreateFileW(own.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        handle != INVALID_HANDLE_VALUE) {
        std::wstring real(MAX_PATH, L'\0');
        DWORD length =
            GetFinalPathNameByHandleW(handle, real.data(), static_cast<DWORD>(real.size()),
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length >= real.size()) {
            real.resize(length);
            length = GetFinalPathNameByHandleW(handle, real.data(), static_cast<DWORD>(real.size()),
                                               FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }

        CloseHandle(handle);
        real.resize(length < real.size() ? length : 0);
        if (real.starts_with(LR"(\\?\UNC\)")) {
            own = LR"(\\)" + real.substr(8);
        } else if (real.starts_with(LR"(\\?\)")) {
            own = real.substr(4);
        } else if (!real.empty()) {
            own = std::move(real);
        }
    }

    const std::size_t slash = own.find_last_of(L"\\/");
    const std::wstring folder = own.substr(0, slash + 1);
    const auto narrow = [](const std::wstring& wide) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(),
                            size, nullptr, nullptr);
        return text;
    };
    const auto widen = [](const std::string_view text) {
        const int size =
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            size);
        return wide;
    };

    // The library takes what it uses from the executable by the name it was built with.
    const std::string named = narrow(own.substr(slash + 1));
    if (_wcsicmp(widen(named).c_str(), widen(executable).c_str()) != 0) {
        stop(std::format("{} was built as {}, which the library {} beside it takes its code from; "
                         "give it that name again",
                         named, executable, file));
    }

    const std::wstring path = folder + widen(file);
    HMODULE library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    // One that can't be loaded may be for an executable that isn't there, which its record says.
    const bool loaded = library != nullptr;
    if (!loaded) {
        library = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    }

    const auto find = [&](const char* name) {
        return reinterpret_cast<void*>(GetProcAddress(library, name));
    };
    const std::string shown = narrow(path);
    const std::string reason;
#else
    std::string own;
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    own.resize(size);
    _NSGetExecutablePath(own.data(), &size);
    own.resize(std::strlen(own.c_str()));
    // Started through a link, it's beside the file it links to.
    if (char* real = realpath(own.c_str(), nullptr)) {
        own = real;
        std::free(real);
    }
#else
    own.resize(4096);
    const ssize_t length = readlink("/proc/self/exe", own.data(), own.size());
    own.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
#endif
    (void)executable;
    const std::string shown = own.substr(0, own.find_last_of('/') + 1) + file;
    // A library beside it that another uses is loaded first, and gives that one its code. One
    // that can't be bound may be another build, which its record says, and says why.
    void* library = dlopen(shown.c_str(), RTLD_NOW | RTLD_GLOBAL);
    const bool loaded = library != nullptr;
    std::string reason;
    if (!loaded) {
        if (const char* error = dlerror()) {
            reason = std::format(" ({})", error);
        }

        library = dlopen(shown.c_str(), RTLD_LAZY | RTLD_LOCAL);
    }

    const auto find = [&](const char* name) { return dlsym(library, name); };
#endif
    if (library == nullptr) {
        stop(std::format("{} isn't there, or can't be loaded; the program needs it beside it{}",
                         shown, reason));
    }

    // It has to be the build the program was built against, by its package's name, and on
    // Windows built for this executable, which it takes its code from.
    const std::string_view wanted(build);
    const std::string record = std::format("fb.(build).{}", wanted.substr(0, wanted.find(' ')));
    const auto* built = static_cast<const char*>(find(record.c_str()));
    const std::string_view made = built != nullptr ? std::string_view(built) : std::string_view();
    const std::size_t beside = made.find(" for ");
    if (built == nullptr || made.substr(0, beside) != wanted) {
        stop(std::format("{} is the build {}, but the program was built against {}", shown,
                         built != nullptr ? made.substr(0, beside)
                                          : "of something other than a FutureBlitz library",
                         build));
    }

#ifdef _WIN32
    if (const std::string_view other =
            beside == std::string_view::npos ? std::string_view() : made.substr(beside + 5);
        _wcsicmp(widen(other).c_str(), widen(executable).c_str()) != 0) {
        stop(std::format("{} was built for {}, which it takes its code from, not {}; build {} "
                         "again",
                         shown, other, executable, executable));
    }
#endif

    if (!loaded) {
        stop(std::format("{} isn't there, or can't be loaded; the program needs it beside it{}",
                         shown, reason));
    }

    for (std::int64_t i = 0; i < count; ++i) {
        slots[i] = find(names[i]);
        if (slots[i] == nullptr) {
            stop(std::format("{} has no {}, which the program uses", shown, names[i]));
        }
    }
}

void fb_exit(const std::int32_t code) {
    // As a panic does, it ends the program with the lock held, after any line another thread is
    // writing.
    ending().lock();
    output().lock();
    end_program(code);
}

std::int64_t fb_process_id() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

void fb_panic_string(const fb_string_t* message, const fb_location_t* location) {
    const auto* bytes = reinterpret_cast<const char*>(message + 1);
    panic_with({ bytes, static_cast<std::size_t>(message->length) }, location);
}

void fb_error_thrown(fb_object_t* error, const fb_location_t* location) {
    fb_error_fields_t& fields = error_fields(error);
    if (fields.line != 0 || location == nullptr) {
        return;
    }

    fb_string_release(fields.file);
    fields.file = fb_string_from_cstring(location->file);
    fields.line = static_cast<std::int32_t>(location->line);
    fields.column = static_cast<std::int32_t>(location->column);
}

void fb_error_uncaught(const fb_object_t* error, const fb_location_t* location) {
    ending().lock();
    output().lock();
    std::fflush(stdout);
    fb::runtime::run_before_report();
    const char* label = fb::runtime::stderr_colors() ? "\x1b[1;31muncaught\x1b[0m" : "uncaught";
    // The Classes shown, so each is named apart from one alike it, as the leak report does.
    std::vector<const fb_class_t*> types;
    for (const fb_object_t* at = error; at != nullptr && types.size() <= max_causes;
         at = error_fields(at).cause) {
        types.push_back(at->type);
    }

    // Then each Cause, as far as the chain goes, which a cycle can't make endless.
    std::size_t shown = 0;
    const fb_object_t* at = error;
    for (; at != nullptr && shown <= max_causes; ++shown) {
        const fb_error_fields_t& fields = error_fields(at);
        std::fprintf(stderr, "%s %s", shown == 0 ? label : "caused by",
                     fb::runtime::shown_class_name(at->type, types));
        if (fields.message->length != 0) {
            std::fputs(": ", stderr);
            std::fwrite(fields.message + 1, 1, static_cast<std::size_t>(fields.message->length),
                        stderr);
        }

        // Where it was thrown, or for one with no Place, where top-level code let it out.
        if (fields.line != 0) {
            std::fputs(" at ", stderr);
            std::fwrite(fields.file + 1, 1, static_cast<std::size_t>(fields.file->length), stderr);
            std::fprintf(stderr, ":%d:%d", fields.line, fields.column);
        } else if (shown == 0 && location != nullptr) {
            std::fprintf(stderr, " at %s:%u:%u", location->file, location->line, location->column);
        }

        std::fputc('\n', stderr);
        at = fields.cause;
    }

    if (at != nullptr) {
        std::fputs("caused by more errors, which aren't shown\n", stderr);
    }

    end_program(error_exit_code);
}

std::int64_t fb_power_int(const std::int64_t base, const std::int64_t exponent,
                          const std::uint32_t bits, const std::int32_t checked,
                          const fb_location_t* location) {
    if (exponent < 0) {
        fb_panic("an integer power needs an exponent of 0 or more", location);
    }

    const auto unsigned_exponent = static_cast<std::uint64_t>(exponent);
    if (checked != 0) {
        // The largest value of a `bits`-wide signed integer.
        const auto max =
            static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() >> (65 - bits));
        std::int64_t result = 0;
        if (power_overflows(base, unsigned_exponent, result) || result > max || result < -max - 1) {
            fb_panic("integer overflow in '**'", location);
        }

        return result;
    }

    const std::uint64_t result =
        wrapping_power(static_cast<std::uint64_t>(base), unsigned_exponent);
    return static_cast<std::int64_t>(truncate(result, bits, true));
}

std::uint64_t fb_power_uint(const std::uint64_t base, const std::uint64_t exponent,
                            const std::uint32_t bits, const std::int32_t checked,
                            const fb_location_t* location) {
    if (checked != 0) {
        const std::uint64_t max = std::numeric_limits<std::uint64_t>::max() >> (64 - bits);
        std::uint64_t result = 0;
        if (power_overflows(base, exponent, result) || result > max) {
            fb_panic("integer overflow in '**'", location);
        }

        return result;
    }

    return truncate(wrapping_power(base, exponent), bits, false);
}

std::uint64_t fb_hash_integer(const std::uint64_t bits) {
    return mix(bits);
}

std::uint64_t fb_hash_float(double value) {
    // 0.0 and -0.0 are equal, so they hash the same.
    if (value == 0.0) {
        value = 0.0;
    }

    return mix(std::bit_cast<std::uint64_t>(value));
}

std::uint64_t fb_hash_combine(const std::uint64_t seed, const std::uint64_t hash) {
    return mix(seed ^ (hash + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U)));
}
}
