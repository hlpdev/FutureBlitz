// std.net's sockets on Linux and macOS, where a test program can't check them on every machine:
// a UDP broadcast, which macOS's Local Network privacy refuses a program it doesn't allow, as on
// CI's runners, so the test says so and skips there; and IPC listeners' Unix socket files, which
// the tests make themselves: one that a program left takes over, another file stays, and a name
// fits however long TMPDIR is. (On Windows, where libuv is compiled for UCRT, which the tests
// aren't, linking it into them breaks their heap.)
#ifndef _WIN32

#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

/// How long a test waits for a connection, in nanoseconds.
constexpr std::int64_t wait_nanoseconds = 5'000'000'000;

/// Ends what the runtime started once the tests are done, as a program's end does, so the I/O
/// thread stops before its std::thread goes.
class finish_runtime_t : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override {
        fb::runtime::run_finish();
    }
};

/// NetErrorKind's ordinals, as fb_io_net_error_kind gives them, HTTP_STATUS's before OTHER.
#ifdef __APPLE__
constexpr std::int32_t unreachable_kind = 2;
#endif
constexpr std::int32_t other_kind = 7;

/// An IPv4 address and port as SocketBytes gives them, in a UInt8[] the test owns, or room for
/// fb_io_net_address's. Its array points at its own bytes, so it isn't copied.
struct address_t {
    fb_array_t array{};
    std::array<std::uint8_t, 23> bytes{};

    address_t(const std::array<std::uint8_t, 4> parts, const std::uint16_t port) {
        array.header.refcount = -1;
        array.count = static_cast<std::int64_t>(bytes.size());
        array.capacity = array.count;
        array.data = bytes.data();
        bytes[0] = 4;
        std::ranges::copy(parts, bytes.begin() + 1);
        bytes[17] = static_cast<std::uint8_t>(port / 256);
        bytes[18] = static_cast<std::uint8_t>(port % 256);
    }

    address_t(const address_t&) = delete;
    address_t& operator=(const address_t&) = delete;
};

/// A UDP socket's port.
std::uint16_t port_of(void* socket) {
    address_t local({}, 0);
    REQUIRE(fb_io_net_address(socket, 0, &local.array) == 0);
    return static_cast<std::uint16_t>((local.bytes[17] * 256) + local.bytes[18]);
}

/// A String the test owns, as a literal is.
struct literal_t {
    fb_string_t header;
    std::array<char, 128> bytes;
};

literal_t literal(const std::string_view text) {
    literal_t made{};
    made.header.refcount = -1;
    made.header.length = static_cast<std::int64_t>(text.size());
    text.copy(made.bytes.data(), made.bytes.size());
    return made;
}

/// The tests' folder in /tmp, and a name in it, of this process's own, since another run of the
/// tests at once on the machine has its own.
std::string own(const std::string_view name) {
    return std::string(name) + "-" + std::to_string(::getpid());
}

/// Makes `path` the folder IPC listeners keep their files in, as TMPDIR, and gives it.
std::string folder(const std::string& path) {
    std::filesystem::create_directories(path);
    ::setenv("TMPDIR", path.c_str(), 1);
    return path;
}

/// Whether a listener at `name` gets a connection there.
bool connects(void* listener, const literal_t& name) {
    void* client = fb_io_net_new(1);
    const bool connected = fb_io_net_connect_pipe(client, &name.header, wait_nanoseconds) == 0 &&
                           fb_io_net_accept(listener, wait_nanoseconds) == 0;
    if (void* accepted = fb_io_net_accepted(listener); accepted != nullptr) {
        fb_io_net_release(accepted);
    }

    fb_io_net_release(client);
    return connected;
}

} // namespace

CATCH_REGISTER_LISTENER(finish_runtime_t)

TEST_CASE("a UDP broadcast needs SetBroadcast, and then what listens at its port gets it") {
    void* receiver = fb_io_net_new(4);
    const address_t any({}, 0);
    REQUIRE(fb_io_net_bind(receiver, &any.array, 1) == 0);
    const address_t everyone({ 255, 255, 255, 255 }, port_of(receiver));
    void* sender = fb_io_net_new(4);
    REQUIRE(fb_io_net_bind(sender, &any.array, 0) == 0);

    const std::array<std::uint8_t, 1> bytes{ 7 };
    const std::int32_t refused = fb_io_net_send(sender, bytes.data(), 1, &everyone.array);
#ifdef __APPLE__
    if (fb_io_net_error_kind(refused) == unreachable_kind) {
        fb_io_net_release(sender);
        fb_io_net_release(receiver);
        SKIP("macOS's Local Network privacy doesn't let this program reach the LAN");
    }
#endif
    CHECK(fb_io_net_error_kind(refused) == other_kind);
    REQUIRE(fb_io_net_set(sender, 0, 1) == 0);
    CHECK(fb_io_net_send(sender, bytes.data(), 1, &everyone.array) == 0);
    CHECK(fb_io_net_receive(receiver, wait_nanoseconds) == 0);
    fb_io_net_release(sender);
    fb_io_net_release(receiver);
}

TEST_CASE("an IPC listener takes over a Unix socket's file that nothing listens at") {
    const std::string path = folder(own("/tmp/fb-net-tests")) + "/fb-net-stale.sock";
    ::unlink(path.c_str());

    // The file a listener leaves when its program ends without closing it.
    const int left = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    path.copy(std::data(address.sun_path), sizeof(address.sun_path) - 1);
    REQUIRE(::bind(left, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(left, 1) == 0);
    ::close(left);

    const literal_t name = literal("fb-net-stale");
    void* listener = fb_io_net_new(3);
    CHECK(fb_io_net_listen_pipe(listener, &name.header, 1) == 0);
    CHECK(connects(listener, name));
    fb_io_net_release(listener);
}

TEST_CASE("an IPC listener leaves a file at its path that isn't a socket") {
    const std::string path = folder(own("/tmp/fb-net-tests")) + "/fb-net-notes.sock";
    std::ofstream{ path } << "notes";

    const literal_t name = literal("fb-net-notes");
    void* listener = fb_io_net_new(3);
    CHECK(fb_io_net_listen_pipe(listener, &name.header, 1) != 0);
    fb_io_net_release(listener);

    struct stat status{};
    REQUIRE(::lstat(path.c_str(), &status) == 0);
    CHECK(S_ISREG(status.st_mode));
    std::ifstream file(path);
    CHECK(std::string(std::istreambuf_iterator<char>(file), {}) == "notes");
    ::unlink(path.c_str());
}

TEST_CASE("an IPC name of 64 bytes works under a TMPDIR too long for its socket's path") {
    // TMPDIR's path to the name's file is longer than a Unix socket's address holds, on Linux
    // and macOS, so the file goes in /tmp.
    folder(own("/tmp/fb-net-tests") + "/" + std::string(48, 'f'));
    std::string text = own("fb-net-long");
    text.resize(64, 'n');
    const literal_t name = literal(text);
    void* listener = fb_io_net_new(3);
    CHECK(fb_io_net_listen_pipe(listener, &name.header, 1) == 0);
    CHECK(connects(listener, name));
    CHECK(std::filesystem::exists("/tmp/" + text + ".sock"));
    fb_io_net_release(listener);
}

#endif
