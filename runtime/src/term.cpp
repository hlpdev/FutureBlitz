// std.term's console: what each stream is, the terminal's size and colors, and stdin read a key
// at a time, with what the program changes put back as it ends, however it ends. Windows' console
// takes and gives virtual terminal sequences once its modes ask for them, and where it gives
// records instead, they're written as a terminal would send them; elsewhere, termios turns echo
// and lines off, and SIGWINCH says the size changed, through a pipe that wakes the read.

#include "fb/term.hpp"

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <deque>
#include <format>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

/// Text that changes the screen, or puts it back, kept where a signal handler can read it.
class screen_text_t {
public:
    void set(const std::string_view text) {
        m_length.store(0, std::memory_order_release);
        const std::size_t count = text.copy(m_bytes.data(), m_bytes.size());
        m_length.store(count, std::memory_order_release);
    }

    /// The text, which is gone after, when `keep` is false.
    [[nodiscard]] std::string_view get(const bool keep) {
        const std::size_t length =
            keep ? m_length.load(std::memory_order_acquire) : m_length.exchange(0);
        return { m_bytes.data(), length };
    }

private:
    std::array<char, 255> m_bytes{};
    std::atomic<std::size_t> m_length = 0;
};

screen_text_t& entered() {
    static screen_text_t text;
    return text;
}

screen_text_t& left() {
    static screen_text_t text;
    return text;
}

/// The mode fb_term_set_mode() last set.
std::atomic<int>& current_mode() {
    static std::atomic<int> mode = 0;
    return mode;
}

/// Writes all of `text` to stdout, as a signal handler can.
void write_out(std::string_view text) {
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    while (!text.empty()) {
        DWORD written = 0;
        if (WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ==
                0 ||
            written == 0) {
            return;
        }

        text.remove_prefix(written);
    }
#else
    while (!text.empty()) {
        const ssize_t written = write(STDOUT_FILENO, text.data(), text.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            return;
        }

        text.remove_prefix(static_cast<std::size_t>(written));
    }
#endif
}

#ifdef _WIN32
/// One of the console's flags, which its macros give as ints, as the DWORD its modes are.
constexpr DWORD bit(const int flag) {
    return static_cast<DWORD>(flag);
}

/// Either Alt key, and either Ctrl key.
constexpr DWORD alt_keys = bit(LEFT_ALT_PRESSED) | bit(RIGHT_ALT_PRESSED);
constexpr DWORD control_keys = bit(LEFT_CTRL_PRESSED) | bit(RIGHT_CTRL_PRESSED);

/// stdin's and stdout's console modes before the program changed them, once it has.
struct modes_t {
    std::atomic<bool> input_saved = false;
    DWORD input = 0;
    std::atomic<bool> output_saved = false;
    DWORD output = 0;
};

modes_t& modes() {
    static modes_t before;
    return before;
}

/// Whether stdin's console gives keys as a terminal sends them, which is what fb_term_set_mode()
/// asks it for first.
std::atomic<bool>& vt_input() {
    static std::atomic<bool> on = false;
    return on;
}

HANDLE handle_of(const std::int32_t stream) {
    switch (stream) {
    case 0:
        return GetStdHandle(STD_INPUT_HANDLE);
    case 1:
        return GetStdHandle(STD_OUTPUT_HANDLE);
    default:
        return GetStdHandle(STD_ERROR_HANDLE);
    }
}
#else
/// stdin's termios before the program changed it, once it has.
struct modes_t {
    std::atomic<bool> saved = false;
    termios input{};
};

modes_t& modes() {
    static modes_t before;
    return before;
}

/// One of termios's flags, which its macros give as ints on some OSes, as the tcflag_t it is.
template <typename Flag>
constexpr tcflag_t bit(const Flag flag) {
    return static_cast<tcflag_t>(flag);
}

/// Sets stdin's termios for `mode`, from what it was before.
void apply_mode(const int mode) {
    termios raw = modes().input;
    raw.c_lflag &= ~(bit(ECHO) | bit(ICANON) | bit(IEXTEN));
    if ((static_cast<std::uint32_t>(mode) & 2U) != 0) {
        raw.c_lflag &= ~bit(ISIG);
    }

    // Enter comes as "\r", and Ctrl+S and Ctrl+Q are keys.
    raw.c_iflag &= ~(bit(IXON) | bit(ICRNL) | bit(INLCR) | bit(ISTRIP) | bit(INPCK) | bit(BRKINT));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/// The pipe a SIGWINCH writes to, so a read waiting on stdin wakes, and whether one came.
struct resize_t {
    std::array<int, 2> pipe{ -1, -1 };
    std::atomic<bool> changed = false;
};

resize_t& resizes() {
    static resize_t resize;
    return resize;
}

void on_resize(int /*number*/) {
    const int saved = errno;
    resizes().changed.store(true);
    if (resizes().pipe[1] >= 0) {
        const char byte = 0;
        [[maybe_unused]] const ssize_t written = write(resizes().pipe[1], &byte, 1);
    }

    errno = saved;
}

/// Ctrl+Z stops the program while the screen is changed: the terminal goes back as it was
/// first, and once the program goes on, it's changed again, and the size counts as changed, so
/// the program draws all of it again.
void on_stop(int /*number*/) {
    const int saved = errno;
    const int mode = current_mode().load();
    if (mode != 0) {
        write_out(left().get(true));
        tcsetattr(STDIN_FILENO, TCSANOW, &modes().input);
    }

    std::signal(SIGTSTP, SIG_DFL);
    sigset_t stop{};
    sigemptyset(&stop);
    sigaddset(&stop, SIGTSTP);
    sigprocmask(SIG_UNBLOCK, &stop, nullptr);
    std::raise(SIGTSTP);

    // The program goes on here, after SIGCONT.
    struct sigaction again{};
    again.sa_handler = on_stop;
    sigemptyset(&again.sa_mask);
    again.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &again, nullptr);
    if (mode != 0) {
        apply_mode(mode);
        write_out(entered().get(true));
    }

    on_resize(SIGWINCH);
    errno = saved;
}

/// Starts watching for SIGWINCH, and for Ctrl+Z where it's at its default, once.
void watch_signals() {
    static std::once_flag once;
    std::call_once(once, [] {
        resize_t& resize = resizes();
        if (pipe(resize.pipe.data()) == 0) {
            for (const int end : resize.pipe) {
                fcntl(end, F_SETFL, fcntl(end, F_GETFL) | O_NONBLOCK);
                fcntl(end, F_SETFD, FD_CLOEXEC);
            }
        } else {
            resize.pipe = { -1, -1 };
        }

        struct sigaction changed{};
        changed.sa_handler = on_resize;
        sigemptyset(&changed.sa_mask);
        changed.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &changed, nullptr);

        struct sigaction current{};
        if (sigaction(SIGTSTP, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
            struct sigaction stopped{};
            stopped.sa_handler = on_stop;
            sigemptyset(&stopped.sa_mask);
            stopped.sa_flags = SA_RESTART;
            sigaction(SIGTSTP, &stopped, nullptr);
        }
    });
}
#endif

/// Puts back what the program changed: the screen first, while stdout still takes escape
/// sequences, then stdin's mode and stdout's. It runs as the program ends, and before a panic's
/// or an uncaught error's message, so it calls only what a signal handler can, and twice does
/// what once does.
void restore_terminal() {
    write_out(left().get(false));
    static_cast<void>(entered().get(false));
    current_mode().store(0);
#ifdef _WIN32
    const modes_t& before = modes();
    if (before.input_saved.load(std::memory_order_acquire)) {
        // The quick edit flag only changes along with the extended flags.
        DWORD input = before.input;
        if ((input & bit(ENABLE_QUICK_EDIT_MODE)) != 0) {
            input |= bit(ENABLE_EXTENDED_FLAGS);
        }

        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), input);
    }

    if (before.output_saved.load(std::memory_order_acquire)) {
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), before.output);
    }
#else
    if (modes().saved.load(std::memory_order_acquire)) {
        tcsetattr(STDIN_FILENO, TCSANOW, &modes().input);
    }
#endif
}

/// Has restore_terminal() run however the program ends, once.
void restore_at_end() {
    static std::once_flag once;
    std::call_once(once, [] {
        fb::runtime::at_end(restore_terminal);
        fb::runtime::before_report(restore_terminal);
    });
}

#ifdef _WIN32
/// What stdin's console gave that's left for the next reads: its records, and the bytes of one
/// that didn't fit, read on whatever thread reads the console.
struct console_input_t {
    std::mutex lock;
    std::deque<INPUT_RECORD> records;
    std::string bytes;
    /// The first half of a character beyond the Basic Multilingual Plane, until its second.
    wchar_t high = 0;
    /// The mouse's buttons, as the last record had them.
    DWORD buttons = 0;
};

console_input_t& console_input() {
    static console_input_t input;
    return input;
}

/// `code`, a character's code point, in UTF-8, `count` times.
void append_utf8(std::string& bytes, const std::uint32_t code, const WORD count) {
    const std::string encoded = fb::runtime::encode_utf8(code);
    for (WORD i = 0; i < std::max<WORD>(count, 1); ++i) {
        bytes += encoded;
    }
}

/// The modifiers of an xterm sequence, 1 for none, plus 1 for Shift, 2 for Alt, and 4 for Ctrl.
int modifiers_of(const DWORD state) {
    int modifiers = 1;
    if ((state & bit(SHIFT_PRESSED)) != 0) {
        modifiers += 1;
    }

    if ((state & alt_keys) != 0) {
        modifiers += 2;
    }

    if ((state & control_keys) != 0) {
        modifiers += 4;
    }

    return modifiers;
}

/// What a terminal sends for a key that has no character, as xterm does, or "" for one it sends
/// nothing for, like Shift alone.
std::string key_sequence(const WORD key, const DWORD state) {
    const int modifiers = modifiers_of(state);
    // A cursor key or F1 to F4 is "\x1b[" and its letter, with its modifiers between.
    const auto lettered = [&](const char letter, const bool ss3) {
        if (modifiers == 1) {
            return std::format("\x1b{}{}", ss3 ? 'O' : '[', letter);
        }

        return std::format("\x1b[1;{}{}", modifiers, letter);
    };
    // The others are a number and "~".
    const auto numbered = [&](const int number) {
        if (modifiers == 1) {
            return std::format("\x1b[{}~", number);
        }

        return std::format("\x1b[{};{}~", number, modifiers);
    };
    switch (key) {
    case VK_UP:
        return lettered('A', false);
    case VK_DOWN:
        return lettered('B', false);
    case VK_RIGHT:
        return lettered('C', false);
    case VK_LEFT:
        return lettered('D', false);
    case VK_HOME:
        return lettered('H', false);
    case VK_END:
        return lettered('F', false);
    case VK_INSERT:
        return numbered(2);
    case VK_DELETE:
        return numbered(3);
    case VK_PRIOR:
        return numbered(5);
    case VK_NEXT:
        return numbered(6);
    case VK_F1:
    case VK_F2:
    case VK_F3:
    case VK_F4:
        return lettered(static_cast<char>('P' + (key - VK_F1)), true);
    case VK_F5:
        return numbered(15);
    case VK_F6:
    case VK_F7:
    case VK_F8:
    case VK_F9:
    case VK_F10:
        return numbered(17 + (key - VK_F6));
    case VK_F11:
    case VK_F12:
        return numbered(23 + (key - VK_F11));
    default:
        return "";
    }
}

/// What a key that went down gives, as a terminal sends it, where `vt` says the console already
/// writes what a terminal would, or else as this writes it.
void translate_key(console_input_t& input, const KEY_EVENT_RECORD& key, const bool vt) {
    if (key.bKeyDown == 0) {
        return;
    }

    const wchar_t unit = key.uChar.UnicodeChar;
    const DWORD state = key.dwControlKeyState;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        input.high = unit;
        return;
    }

    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (input.high != 0) {
            const std::uint32_t code = 0x10000U +
                                       ((static_cast<std::uint32_t>(input.high) - 0xD800U) << 10U) +
                                       (static_cast<std::uint32_t>(unit) - 0xDC00U);
            append_utf8(input.bytes, code, key.wRepeatCount);
        }

        input.high = 0;
        return;
    }

    input.high = 0;
    if (unit != 0) {
        // A console that doesn't send what a terminal would writes Alt and Shift+Tab itself.
        if (!vt) {
            if (unit == L'\t' && (state & bit(SHIFT_PRESSED)) != 0) {
                input.bytes += "\x1b[Z";
                return;
            }

            if ((state & alt_keys) != 0 && (state & control_keys) == 0) {
                input.bytes += '\x1b';
            }
        }

        append_utf8(input.bytes, unit, key.wRepeatCount);
        return;
    }

    // Ctrl+Space is NUL, which the console gives as no character.
    if (key.wVirtualKeyCode == VK_SPACE && (state & control_keys) != 0) {
        input.bytes += '\0';
        return;
    }

    if (!vt) {
        const std::string sequence = key_sequence(key.wVirtualKeyCode, state);
        for (WORD i = 0; i < std::max<WORD>(key.wRepeatCount, 1); ++i) {
            input.bytes += sequence;
        }
    }
}

/// What the mouse did, as a terminal reports it with SGR's numbers: a button, 0 to 2, plus 32 for
/// a move, 64 and up for a wheel, and 4, 8, and 16 for Shift, Alt, and Ctrl, then the column and
/// row from 1, and "M" for a press, or "m" for a release.
void translate_mouse(console_input_t& input, const MOUSE_EVENT_RECORD& mouse) {
    int code = 0;
    bool released = false;
    const DWORD buttons = mouse.dwButtonState & 0xFFFFU;
    if ((mouse.dwEventFlags & (bit(MOUSE_WHEELED) | bit(MOUSE_HWHEELED))) != 0) {
        // Up and down are 64 and 65, and left and right 66 and 67.
        const bool forward = static_cast<std::int16_t>(HIWORD(mouse.dwButtonState)) > 0;
        code = forward ? 64 : 65;
        if ((mouse.dwEventFlags & bit(MOUSE_HWHEELED)) != 0) {
            code = forward ? 67 : 66;
        }
    } else {
        // Each button by the bit the console gives it: left, then right, then middle.
        const auto button_of = [](const DWORD bits) {
            if ((bits & bit(FROM_LEFT_1ST_BUTTON_PRESSED)) != 0) {
                return 0;
            }

            if ((bits & bit(FROM_LEFT_2ND_BUTTON_PRESSED)) != 0) {
                return 1;
            }

            return (bits & bit(RIGHTMOST_BUTTON_PRESSED)) != 0 ? 2 : 3;
        };
        if ((mouse.dwEventFlags & bit(MOUSE_MOVED)) != 0) {
            // A move with no button down is reported only where every move is asked for, which
            // std.term doesn't ask.
            if (button_of(buttons) == 3) {
                input.buttons = buttons;
                return;
            }

            code = button_of(buttons) + 32;
        } else if ((buttons & ~input.buttons) != 0) {
            code = button_of(buttons & ~input.buttons);
        } else {
            code = button_of(input.buttons & ~buttons);
            released = true;
        }

        input.buttons = buttons;
        // What changes no button, as a double click's second press can, is nothing.
        if (code == 3) {
            return;
        }
    }

    const DWORD state = mouse.dwControlKeyState;
    if ((state & bit(SHIFT_PRESSED)) != 0) {
        code += 4;
    }

    if ((state & alt_keys) != 0) {
        code += 8;
    }

    if ((state & control_keys) != 0) {
        code += 16;
    }

    // The console counts rows in its buffer, from the window's top.
    SHORT top = 0;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) != 0) {
        top = info.srWindow.Top;
    }

    input.bytes += std::format("\x1b[<{};{};{}{}", code, mouse.dwMousePosition.X + 1,
                               mouse.dwMousePosition.Y - top + 1, released ? 'm' : 'M');
}

/// Gives the bytes there are, as far as `into` has room, keeping the rest.
std::int64_t give(std::string& bytes, char* into, const std::int64_t room) {
    const auto count = std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(room));
    std::copy_n(bytes.data(), count, into);
    bytes.erase(0, count);
    return static_cast<std::int64_t>(count);
}

/// How long until `deadline`, in whole milliseconds, rounded up, or INFINITE for no deadline.
DWORD milliseconds_until(const std::chrono::steady_clock::time_point deadline, const bool forever) {
    if (forever) {
        return INFINITE;
    }

    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return static_cast<DWORD>(std::clamp<std::int64_t>(left.count(), 0, 0x7FFFFFFF));
}

std::int64_t read_console(HANDLE handle, char* into, const std::int64_t room,
                          const std::chrono::steady_clock::time_point deadline,
                          const bool forever) {
    console_input_t& input = console_input();
    const std::scoped_lock lock(input.lock);
    for (;;) {
        // The records in order, as many as have come: bytes before a resize go first, and a
        // resize before the bytes after it.
        while (!input.records.empty()) {
            const INPUT_RECORD record = input.records.front();
            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                if (!input.bytes.empty()) {
                    break;
                }

                input.records.pop_front();
                return -2;
            }

            input.records.pop_front();
            if (record.EventType == KEY_EVENT) {
                translate_key(input, record.Event.KeyEvent, vt_input().load());
            } else if (record.EventType == MOUSE_EVENT) {
                translate_mouse(input, record.Event.MouseEvent);
            }
        }

        if (!input.bytes.empty()) {
            return give(input.bytes, into, room);
        }

        const DWORD waited = WaitForSingleObject(handle, milliseconds_until(deadline, forever));
        if (waited == WAIT_TIMEOUT) {
            return 0;
        }

        if (waited != WAIT_OBJECT_0) {
            return -1;
        }

        std::array<INPUT_RECORD, 64> records{};
        DWORD count = 0;
        if (ReadConsoleInputW(handle, records.data(), static_cast<DWORD>(records.size()), &count) ==
            0) {
            return -1;
        }

        input.records.insert(input.records.end(), records.begin(),
                             records.begin() + static_cast<std::ptrdiff_t>(count));
    }
}

/// A pipe's or a file's bytes, waiting for a pipe's until the deadline.
std::int64_t read_other(HANDLE handle, char* into, const std::int64_t room,
                        const std::chrono::steady_clock::time_point deadline, const bool forever) {
    if (GetFileType(handle) == FILE_TYPE_PIPE && !forever) {
        for (;;) {
            DWORD ready = 0;
            if (PeekNamedPipe(handle, nullptr, 0, nullptr, &ready, nullptr) == 0) {
                return -1;
            }

            if (ready > 0) {
                break;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                return 0;
            }

            Sleep(1);
        }
    }

    DWORD count = 0;
    const auto wanted = static_cast<DWORD>(std::min<std::int64_t>(room, 0x40000000));
    if (ReadFile(handle, into, wanted, &count, nullptr) == 0 || count == 0) {
        return -1;
    }

    return count;
}
#endif

} // namespace

#ifdef _WIN32
std::string fb::term::key_bytes(const std::span<const key_record_t> records, const bool vt) {
    console_input_t input;
    for (const key_record_t& record : records) {
        KEY_EVENT_RECORD key{};
        key.bKeyDown = record.down ? TRUE : FALSE;
        key.wRepeatCount = record.repeat;
        key.wVirtualKeyCode = record.key;
        key.uChar.UnicodeChar = static_cast<wchar_t>(record.unit);
        key.dwControlKeyState = record.state;
        translate_key(input, key, vt);
    }

    return input.bytes;
}

std::string fb::term::mouse_bytes(const std::span<const mouse_record_t> records) {
    console_input_t input;
    for (const mouse_record_t& record : records) {
        MOUSE_EVENT_RECORD mouse{};
        mouse.dwButtonState = record.buttons;
        mouse.dwEventFlags = record.flags;
        mouse.dwMousePosition = { .X = record.column, .Y = record.row };
        mouse.dwControlKeyState = record.state;
        translate_mouse(input, mouse);
    }

    return input.bytes;
}
#endif

int fb::term::color_level(const bool terminal, const char* no_color, const char* term,
                          const char* color_term, const bool windows) {
    const auto is = [](const char* value, const std::string_view text) {
        return value != nullptr && text == value;
    };
    if (!terminal || (no_color != nullptr && *no_color != '\0') || is(term, "dumb")) {
        return 0;
    }

    if (is(color_term, "truecolor") || is(color_term, "24bit")) {
        return 3;
    }

    if (term != nullptr && std::string_view(term).contains("256color")) {
        return 2;
    }

    // Windows' console has shown 24-bit color since it took escape sequences.
    return windows ? 3 : 1;
}

extern "C" {

std::int32_t fb_term_is_terminal(const std::int32_t stream) {
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(handle_of(stream), &mode) != 0 ? 1 : 0;
#else
    return isatty(stream) != 0 ? 1 : 0;
#endif
}

std::int32_t fb_term_size(std::int32_t* columns, std::int32_t* rows) {
#ifdef _WIN32
    for (const std::int32_t stream : { 1, 2 }) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(handle_of(stream), &info) != 0) {
            *columns = info.srWindow.Right - info.srWindow.Left + 1;
            *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
            return 1;
        }
    }
#else
    for (const int stream : { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO }) {
        winsize size{};
        if (ioctl(stream, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
            *columns = size.ws_col;
            *rows = size.ws_row;
            return 1;
        }
    }
#endif
    return 0;
}

std::int32_t fb_term_start(const std::int32_t stream) {
#ifdef _WIN32
    HANDLE handle = handle_of(stream);
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode) == 0) {
        return 0;
    }

    if ((mode & bit(ENABLE_VIRTUAL_TERMINAL_PROCESSING)) != 0) {
        return 1;
    }

    // stdout gets its mode back as the program ends; fb_start() did stderr's already.
    if (stream == 1) {
        modes_t& before = modes();
        if (!before.output_saved.load(std::memory_order_acquire)) {
            before.output = mode;
            before.output_saved.store(true, std::memory_order_release);
            restore_at_end();
        }
    }

    return SetConsoleMode(handle, mode | bit(ENABLE_VIRTUAL_TERMINAL_PROCESSING)) != 0 ? 1 : 0;
#else
    return isatty(stream) != 0 ? 1 : 0;
#endif
}

std::int32_t fb_term_colors(const std::int32_t stream) {
#ifdef _WIN32
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    return fb::term::color_level(fb_term_start(stream) != 0, std::getenv("NO_COLOR"),
                                 std::getenv("TERM"), std::getenv("COLORTERM"), windows);
}

std::int32_t fb_term_set_mode(const std::int32_t mode) {
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD now = 0;
    if (GetConsoleMode(input, &now) == 0) {
        return 0;
    }

    modes_t& before = modes();
    if (!before.input_saved.load(std::memory_order_acquire)) {
        before.input = now;
        before.input_saved.store(true, std::memory_order_release);
        restore_at_end();
    }

    current_mode().store(mode);
    if (mode == 0) {
        vt_input().store(false);
        DWORD original = before.input;
        if ((original & bit(ENABLE_QUICK_EDIT_MODE)) != 0) {
            original |= bit(ENABLE_EXTENDED_FLAGS);
        }

        SetConsoleMode(input, original);
        return 1;
    }

    DWORD raw = (before.input &
                 ~(bit(ENABLE_ECHO_INPUT) | bit(ENABLE_LINE_INPUT) | bit(ENABLE_MOUSE_INPUT))) |
                bit(ENABLE_WINDOW_INPUT);
    if ((static_cast<std::uint32_t>(mode) & 2U) != 0) {
        raw &= ~bit(ENABLE_PROCESSED_INPUT);
    } else {
        raw |= bit(ENABLE_PROCESSED_INPUT);
    }

    // The mouse's reports need quick edit off, or a drag selects text instead.
    if ((static_cast<std::uint32_t>(mode) & 4U) != 0) {
        raw = (raw & ~bit(ENABLE_QUICK_EDIT_MODE)) | bit(ENABLE_MOUSE_INPUT) |
              bit(ENABLE_EXTENDED_FLAGS);
    }

    const bool vt = SetConsoleMode(input, raw | bit(ENABLE_VIRTUAL_TERMINAL_INPUT)) != 0;
    vt_input().store(vt);
    if (!vt) {
        SetConsoleMode(input, raw);
    }

    return 1;
#else
    if (isatty(STDIN_FILENO) == 0) {
        return 0;
    }

    modes_t& before = modes();
    if (!before.saved.load(std::memory_order_acquire)) {
        if (tcgetattr(STDIN_FILENO, &before.input) != 0) {
            return 0;
        }

        before.saved.store(true, std::memory_order_release);
        restore_at_end();
    }

    watch_signals();
    current_mode().store(mode);
    if (mode == 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &before.input);
    } else {
        apply_mode(mode);
    }

    return 1;
#endif
}

std::int64_t fb_term_read(const fb_array_t* into, const std::int64_t timeout) {
    auto* bytes = static_cast<char*>(into->data);
    const std::int64_t room = into->count;
    const bool forever = timeout < 0;
    const auto deadline = forever ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() +
                                        std::chrono::ceil<std::chrono::steady_clock::duration>(
                                            std::chrono::nanoseconds(timeout));
    if (room <= 0) {
        return 0;
    }

#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) {
        return -1;
    }

    DWORD mode = 0;
    if (GetConsoleMode(input, &mode) != 0) {
        return read_console(input, bytes, room, deadline, forever);
    }

    return read_other(input, bytes, room, deadline, forever);
#else
    watch_signals();
    resize_t& resize = resizes();
    for (;;) {
        if (resize.changed.exchange(false)) {
            // What woke the read goes, however many resizes came.
            std::array<char, 64> drained{};
            ssize_t count = 0;
            do {
                count =
                    resize.pipe[0] >= 0 ? read(resize.pipe[0], drained.data(), drained.size()) : 0;
            } while (count > 0);

            return -2;
        }

        // select, since macOS's poll doesn't take a terminal.
        timeval wait{};
        if (!forever) {
            const auto left = std::chrono::ceil<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now());
            const std::int64_t micros = std::max<std::int64_t>(left.count(), 0);
            wait.tv_sec = static_cast<decltype(wait.tv_sec)>(micros / 1000000);
            wait.tv_usec = static_cast<decltype(wait.tv_usec)>(micros % 1000000);
        }

        fd_set watched{};
        FD_ZERO(&watched);
        FD_SET(STDIN_FILENO, &watched);
        if (resize.pipe[0] >= 0) {
            FD_SET(resize.pipe[0], &watched);
        }

        const int ready = select(std::max(STDIN_FILENO, resize.pipe[0]) + 1, &watched, nullptr,
                                 nullptr, forever ? nullptr : &wait);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (ready == 0) {
            if (!forever && std::chrono::steady_clock::now() >= deadline) {
                return 0;
            }

            continue;
        }

        if (!FD_ISSET(STDIN_FILENO, &watched)) {
            continue;
        }

        const ssize_t count = read(STDIN_FILENO, bytes, static_cast<std::size_t>(room));
        if (count > 0) {
            return count;
        }

        if (count == 0) {
            return -1;
        }

        // A stdin that libuv made non-blocking has nothing yet.
        if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
#endif
}

void fb_term_set_screen(const fb_string_t* enter, const fb_string_t* leave) {
    restore_at_end();
    entered().set(fb::runtime::text_of(enter));
    left().set(fb::runtime::text_of(leave));
}
}
