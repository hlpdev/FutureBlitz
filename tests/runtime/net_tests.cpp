// std.net's IPC listeners on Linux and macOS, where they're Unix sockets whose files the tests
// make themselves: one that a program left takes over, another file stays, and a name fits
// however long TMPDIR is. Windows' named pipes have no files.
#ifndef _WIN32

#include "fb/runtime.hpp"
#include "internal.hpp"

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

TEST_CASE("an IPC listener takes over a Unix socket's file that nothing listens at") {
    const std::string path = folder("/tmp/fb-net-tests") + "/fb-net-stale.sock";
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
    const std::string path = folder("/tmp/fb-net-tests") + "/fb-net-notes.sock";
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
    folder("/tmp/fb-net-tests/" + std::string(48, 'f'));
    const std::string text = "fb-net-long-" + std::string(52, 'n');
    const literal_t name = literal(text);
    void* listener = fb_io_net_new(3);
    CHECK(fb_io_net_listen_pipe(listener, &name.header, 1) == 0);
    CHECK(connects(listener, name));
    CHECK(std::filesystem::exists("/tmp/" + text + ".sock"));
    fb_io_net_release(listener);
}

#endif
