// Registries' tokens in the OS's own credential store: Windows' Credential Manager, the macOS
// keychain, or libsecret's Secret Service on Linux, which fb loads as it needs it, so fb runs
// on a machine without it.

#include "fb/credentials.hpp"

#include "fb/registry_api.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// wincred.h needs windows.h's types first.
#include <wincred.h>
#elifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <dlfcn.h>
#endif

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fb {
namespace {

/// The service a token is kept under, in stores that name one beside the key.
constexpr std::string_view service = "futureblitz";

#ifdef _WIN32

/// Windows' Credential Manager: a generic credential for each key, kept for this user.
class windows_store_t final : public credential_store_t {
public:
    std::expected<std::optional<std::string>, std::string>
    get(const std::string_view key) override {
        const std::wstring target = path_from_utf8(key).wstring();
        PCREDENTIALW found = nullptr;
        if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &found) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_NOT_FOUND) {
                return std::nullopt;
            }

            return std::unexpected(
                std::format("Windows' Credential Manager couldn't be read (error {})", error));
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): its bytes are the token's
        std::string token(reinterpret_cast<const char*>(found->CredentialBlob),
                          found->CredentialBlobSize);
        CredFree(found);
        return token;
    }

    std::expected<void, std::string> set(const std::string_view key,
                                         const std::string_view token) override {
        std::wstring target = path_from_utf8(key).wstring();
        std::wstring user = L"fb";
        std::string blob(token);
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = target.data();
        credential.UserName = user.data();
        credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): it takes bytes
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (CredWriteW(&credential, 0) == FALSE) {
            return std::unexpected(std::format(
                "Windows' Credential Manager couldn't keep the token (error {})", GetLastError()));
        }

        return {};
    }

    std::expected<bool, std::string> remove(const std::string_view key) override {
        const std::wstring target = path_from_utf8(key).wstring();
        if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_NOT_FOUND) {
                return false;
            }

            return std::unexpected(std::format(
                "Windows' Credential Manager couldn't remove the token (error {})", error));
        }

        return true;
    }
};

#elifdef __APPLE__

/// Releases a Core Foundation object as it goes.
struct cf_release_t {
    void operator()(const void* object) const {
        if (object != nullptr) {
            CFRelease(object);
        }
    }
};

template <typename T>
using cf_t = std::unique_ptr<std::remove_pointer_t<T>, cf_release_t>;

cf_t<CFStringRef> cf_string(const std::string_view text) {
    return cf_t<CFStringRef>(
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false));
}

/// The macOS keychain: a generic password for each key, under the service "futureblitz".
class keychain_store_t final : public credential_store_t {
public:
    std::expected<std::optional<std::string>, std::string>
    get(const std::string_view key) override {
        const cf_t<CFMutableDictionaryRef> query = query_of(key);
        CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef found = nullptr;
        const OSStatus status = SecItemCopyMatching(query.get(), &found);
        if (status == errSecItemNotFound) {
            return std::nullopt;
        }

        if (status != errSecSuccess || found == nullptr) {
            return std::unexpected(
                std::format("the keychain couldn't be read (OSStatus {})", status));
        }

        const cf_t<CFDataRef> data(static_cast<CFDataRef>(found));
        return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data.get())),
                           static_cast<std::size_t>(CFDataGetLength(data.get())));
    }

    std::expected<void, std::string> set(const std::string_view key,
                                         const std::string_view token) override {
        const cf_t<CFDataRef> data(CFDataCreate(kCFAllocatorDefault,
                                                reinterpret_cast<const UInt8*>(token.data()),
                                                static_cast<CFIndex>(token.size())));
        const cf_t<CFMutableDictionaryRef> query = query_of(key);
        const cf_t<CFMutableDictionaryRef> change(
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
        CFDictionarySetValue(change.get(), kSecValueData, data.get());
        OSStatus status = SecItemUpdate(query.get(), change.get());
        if (status == errSecItemNotFound) {
            CFDictionarySetValue(query.get(), kSecValueData, data.get());
            status = SecItemAdd(query.get(), nullptr);
        }

        if (status != errSecSuccess) {
            return std::unexpected(
                std::format("the keychain couldn't keep the token (OSStatus {})", status));
        }

        return {};
    }

    std::expected<bool, std::string> remove(const std::string_view key) override {
        const OSStatus status = SecItemDelete(query_of(key).get());
        if (status == errSecItemNotFound) {
            return false;
        }

        if (status != errSecSuccess) {
            return std::unexpected(
                std::format("the keychain couldn't remove the token (OSStatus {})", status));
        }

        return true;
    }

private:
    /// What finds a key's item: a generic password of the service, by the key as its account.
    static cf_t<CFMutableDictionaryRef> query_of(const std::string_view key) {
        cf_t<CFMutableDictionaryRef> query(
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
        const cf_t<CFStringRef> named = cf_string(service);
        const cf_t<CFStringRef> account = cf_string(key);
        CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query.get(), kSecAttrService, named.get());
        CFDictionarySetValue(query.get(), kSecAttrAccount, account.get());
        return query;
    }
};

#else

// libsecret's and GLib's types, as their headers have them, which fb doesn't build against.
using gboolean = int;
using gchar = char;

struct gerror_t {
    unsigned domain;
    int code;
    gchar* message;
};

struct schema_attribute_t {
    const gchar* name;
    int type;
};

struct secret_schema_t {
    const gchar* name;
    int flags;
    schema_attribute_t attributes[32];
    int reserved;
    void* reserved1;
    void* reserved2;
    void* reserved3;
    void* reserved4;
    void* reserved5;
    void* reserved6;
    void* reserved7;
};

using store_fn_t = gboolean (*)(const secret_schema_t*, const gchar*, const gchar*, const gchar*,
                                void*, gerror_t**, ...);
using lookup_fn_t = gchar* (*)(const secret_schema_t*, void*, gerror_t**, ...);
using clear_fn_t = gboolean (*)(const secret_schema_t*, void*, gerror_t**, ...);
using free_fn_t = void (*)(gchar*);
using error_free_fn_t = void (*)(gerror_t*);

/// The Secret Service, through libsecret, loaded once, the first time a token is kept or read.
class secret_store_t final : public credential_store_t {
public:
    std::expected<std::optional<std::string>, std::string>
    get(const std::string_view key) override {
        if (const std::optional<std::string> missing = load()) {
            return std::unexpected(*missing);
        }

        gerror_t* error = nullptr;
        const std::string account(key);
        gchar* found = m_lookup(&m_schema, nullptr, &error, "service", service.data(), "registry",
                                account.c_str(), nullptr);
        if (error != nullptr) {
            return std::unexpected(failed("read", error));
        }

        if (found == nullptr) {
            return std::nullopt;
        }

        std::string token(found);
        m_free(found);
        return token;
    }

    std::expected<void, std::string> set(const std::string_view key,
                                         const std::string_view token) override {
        if (const std::optional<std::string> missing = load()) {
            return std::unexpected(*missing);
        }

        gerror_t* error = nullptr;
        const std::string account(key);
        const std::string label = std::format("FutureBlitz registry token ({})", key);
        const std::string password(token);
        const gboolean stored =
            m_store(&m_schema, nullptr, label.c_str(), password.c_str(), nullptr, &error, "service",
                    service.data(), "registry", account.c_str(), nullptr);
        if (error != nullptr || stored == 0) {
            return std::unexpected(failed("keep the token in", error));
        }

        return {};
    }

    std::expected<bool, std::string> remove(const std::string_view key) override {
        if (const std::optional<std::string> missing = load()) {
            return std::unexpected(*missing);
        }

        gerror_t* error = nullptr;
        const std::string account(key);
        const gboolean cleared = m_clear(&m_schema, nullptr, &error, "service", service.data(),
                                         "registry", account.c_str(), nullptr);
        if (error != nullptr) {
            return std::unexpected(failed("remove the token from", error));
        }

        return cleared != 0;
    }

private:
    void* m_library = nullptr;
    store_fn_t m_store = nullptr;
    lookup_fn_t m_lookup = nullptr;
    clear_fn_t m_clear = nullptr;
    free_fn_t m_free = nullptr;
    error_free_fn_t m_error_free = nullptr;
    secret_schema_t m_schema{
        .name = "dev.futureblitz.Token",
        .flags = 0,
        .attributes = { { .name = "service", .type = 0 }, { .name = "registry", .type = 0 } },
    };

    /// Loads libsecret, or says why it can't.
    std::optional<std::string> load() {
        if (m_library != nullptr) {
            return std::nullopt;
        }

        void* library = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            return "there's no libsecret on this machine for keeping tokens; FB_CREDENTIALS_FILE "
                   "names a file to keep them in instead, or FB_REGISTRY_TOKEN gives one";
        }

        m_store = reinterpret_cast<store_fn_t>(dlsym(library, "secret_password_store_sync"));
        m_lookup = reinterpret_cast<lookup_fn_t>(dlsym(library, "secret_password_lookup_sync"));
        m_clear = reinterpret_cast<clear_fn_t>(dlsym(library, "secret_password_clear_sync"));
        m_free = reinterpret_cast<free_fn_t>(dlsym(library, "secret_password_free"));
        m_error_free = reinterpret_cast<error_free_fn_t>(dlsym(library, "g_error_free"));
        if (m_store == nullptr || m_lookup == nullptr || m_clear == nullptr || m_free == nullptr ||
            m_error_free == nullptr) {
            dlclose(library);
            return "this machine's libsecret lacks what fb uses; FB_CREDENTIALS_FILE names a file "
                   "to keep tokens in instead, or FB_REGISTRY_TOKEN gives one";
        }

        m_library = library;
        return std::nullopt;
    }

    std::string failed(const std::string_view what, gerror_t* error) const {
        std::string message = std::format(
            "the Secret Service couldn't {} it{}{}; without one, FB_CREDENTIALS_FILE names a "
            "file to keep tokens in, or FB_REGISTRY_TOKEN gives one",
            what, error != nullptr && error->message != nullptr ? ": " : "",
            error != nullptr && error->message != nullptr ? error->message : "");
        if (error != nullptr) {
            m_error_free(error);
        }

        return message;
    }
};

#endif

/// Tokens in a JSON file, each by its key, which only its owner can read.
class file_store_t final : public credential_store_t {
public:
    explicit file_store_t(std::filesystem::path path) : m_path(std::move(path)) {}

    std::expected<std::optional<std::string>, std::string>
    get(const std::string_view key) override {
        const std::expected<nlohmann::json, std::string> tokens = read();
        if (!tokens) {
            return std::unexpected(tokens.error());
        }

        const auto found = tokens->find(key);
        if (found == tokens->end() || !found->is_string()) {
            return std::nullopt;
        }

        return found->get<std::string>();
    }

    std::expected<void, std::string> set(const std::string_view key,
                                         const std::string_view token) override {
        if (const std::optional<std::string> wrong = token_error(token)) {
            return std::unexpected(*wrong);
        }

        std::expected<nlohmann::json, std::string> tokens = read();
        if (!tokens) {
            return std::unexpected(tokens.error());
        }

        tokens.value()[std::string(key)] = std::string(token);
        return write(*tokens);
    }

    std::expected<bool, std::string> remove(const std::string_view key) override {
        std::expected<nlohmann::json, std::string> tokens = read();
        if (!tokens) {
            return std::unexpected(tokens.error());
        }

        if (tokens->erase(std::string(key)) == 0) {
            return false;
        }

        return write(*tokens).transform([] { return true; });
    }

private:
    std::filesystem::path m_path;

    std::expected<nlohmann::json, std::string> read() const {
        const std::ifstream in(m_path, std::ios::binary);
        if (!in) {
            return nlohmann::json::object();
        }

        std::ostringstream text;
        text << in.rdbuf();
        // An empty file, as one made to be private first is, has no tokens yet.
        if (text.view().find_first_not_of(" \t\r\n") == std::string_view::npos) {
            return nlohmann::json::object();
        }

        nlohmann::json tokens = nlohmann::json::parse(text.str(), nullptr, false);
        if (!tokens.is_object()) {
            return std::unexpected(
                std::format("{} isn't a JSON object of tokens", to_utf8(m_path)));
        }

        return tokens;
    }

    /// Writes the file whole, to a file beside it first that only its owner can read.
    std::expected<void, std::string> write(const nlohmann::json& tokens) const {
        std::error_code error;
        if (m_path.has_parent_path()) {
            std::filesystem::create_directories(m_path.parent_path(), error);
        }

        // Tokens are printable ASCII, which set() checks, so this doesn't throw.
        const std::string text = tokens.dump(2, ' ', true) + '\n';
        std::random_device random;
        std::filesystem::path partial = m_path;
        partial += std::format(".partial-{:08x}", random());
        if (!write_private(partial, text)) {
            std::filesystem::remove(partial, error);
            return std::unexpected(std::format("couldn't write {}", to_utf8(m_path)));
        }

        std::filesystem::rename(partial, m_path, error);
        if (error) {
            std::filesystem::remove(partial, error);
            return std::unexpected(
                std::format("couldn't write {}: {}", to_utf8(m_path), error.message()));
        }

        return {};
    }

    /// Writes `text` to a new file at `path` that only its owner can read from the start.
    static bool write_private(const std::filesystem::path& path, const std::string_view text) {
#ifdef _WIN32
        // A file in the user's own folders is theirs alone already.
        std::ofstream out(path, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        return static_cast<bool>(out);
#else
        const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (file < 0) {
            return false;
        }

        std::size_t done = 0;
        while (done < text.size()) {
            const ssize_t written = ::write(file, text.data() + done, text.size() - done);
            if (written < 0 && errno == EINTR) {
                continue;
            }

            if (written <= 0) {
                close(file);
                return false;
            }

            done += static_cast<std::size_t>(written);
        }

        return close(file) == 0;
#endif
    }
};

} // namespace

std::unique_ptr<credential_store_t> file_credential_store(const std::filesystem::path& path) {
    return std::make_unique<file_store_t>(path);
}

std::unique_ptr<credential_store_t> credential_store() {
    if (const std::optional<std::string> file = environment_variable("FB_CREDENTIALS_FILE");
        file && !file->empty()) {
        return file_credential_store(path_from_utf8(*file));
    }

    return os_credential_store();
}

std::unique_ptr<credential_store_t> os_credential_store() {
#ifdef _WIN32
    return std::make_unique<windows_store_t>();
#elifdef __APPLE__
    return std::make_unique<keychain_store_t>();
#else
    return std::make_unique<secret_store_t>();
#endif
}

std::optional<std::string> token_error(const std::string_view token) {
    if (token.empty()) {
        return "a token can't be empty";
    }

    if (!std::ranges::all_of(token, [](const char c) { return c > ' ' && c < 0x7F; })) {
        return "that isn't a registry's token, which is printable ASCII without spaces";
    }

    return std::nullopt;
}

std::string credential_key(const std::string_view index) {
    return std::format("{}:{}", service, index);
}

} // namespace fb
