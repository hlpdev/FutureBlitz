#include "fb/os.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

// std.os's paths apart from the OS; tests/programs' os_ programs check what each OS gives.

TEST_CASE("a URL starts with its scheme, which a drive's letter isn't", "[runtime]") {
    CHECK(fb::os::is_url("https://futureblitz.dev"));
    CHECK(fb::os::is_url("mailto:someone@futureblitz.dev"));
    CHECK(fb::os::is_url("git+ssh://host/repo"));
    CHECK(fb::os::is_url("x-custom.app:open"));
    CHECK(!fb::os::is_url("C:/Users/x/file.txt"));
    CHECK(!fb::os::is_url("c:\\file.txt"));
    CHECK(!fb::os::is_url("notes.txt"));
    CHECK(!fb::os::is_url("folder/a:b"));
    CHECK(!fb::os::is_url("1http://x"));
    CHECK(!fb::os::is_url(":x"));
    CHECK(!fb::os::is_url(""));
}

TEST_CASE("an XDG variable names a folder when it's an absolute path", "[runtime]") {
    const std::optional<std::string> home = "/home/someone";
    CHECK(fb::os::xdg_folder("/xdg/config", home, ".config") == "/xdg/config");
    CHECK(fb::os::xdg_folder(std::nullopt, home, ".config") == "/home/someone/.config");
    CHECK(fb::os::xdg_folder("relative/cache", home, ".cache") == "/home/someone/.cache");
    CHECK(fb::os::xdg_folder("", home, ".local/share") == "/home/someone/.local/share");
    CHECK(fb::os::xdg_folder("/xdg/data", std::nullopt, ".local/share") == "/xdg/data");
    CHECK(fb::os::xdg_folder(std::nullopt, std::nullopt, ".config") == std::nullopt);
}

TEST_CASE("user-dirs.dirs names the documents folder in home or absolutely", "[runtime]") {
    const auto documents = [](const std::string& text) {
        return fb::os::user_dirs_documents(text, "/home/someone");
    };
    CHECK(documents("# This file is written by xdg-user-dirs-update\n"
                    "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\n"
                    "XDG_DOCUMENTS_DIR=\"$HOME/My Documents\"\n"
                    "XDG_MUSIC_DIR=\"$HOME/Music\"\n") == "/home/someone/My Documents");
    CHECK(documents("XDG_DOCUMENTS_DIR=\"/srv/documents\"") == "/srv/documents");
    CHECK(documents("XDG_DOCUMENTS_DIR=\"$HOME\"\n") == "/home/someone");
    // The last line counts, as when a shell reads the file.
    CHECK(documents("XDG_DOCUMENTS_DIR=\"$HOME/a\"\nXDG_DOCUMENTS_DIR=\"$HOME/b\"\n") ==
          "/home/someone/b");
    // What isn't one of xdg-user-dirs' two forms names nothing.
    CHECK(documents("") == std::nullopt);
    CHECK(documents("XDG_DESKTOP_DIR=\"$HOME/Desktop\"\n") == std::nullopt);
    CHECK(documents("XDG_DOCUMENTS_DIR=\"relative\"\n") == std::nullopt);
    CHECK(documents("XDG_DOCUMENTS_DIR=\"$HOMEWORK/x\"\n") == std::nullopt);
    CHECK(documents("XDG_DOCUMENTS_DIR=\"$HOME/no end\n") == std::nullopt);
    CHECK(documents("  XDG_DOCUMENTS_DIR=\"$HOME/indented\"\n") == std::nullopt);
}
