// std.db's functions over SQLite: a connection, the statements it prepares, their parameters,
// and the rows they step through. SQLite is built thread-safe, so a connection can be used from
// any thread, one call at a time.

#include "fb/db.hpp"

#include "fb/runtime.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sqlite3.h>
#include <string>

namespace {

/// What fb_db_prepare gives for SQL that isn't one statement, what any call that reads SQL or a
/// path gives for one that holds a NUL, and what fb_db_run gives for a statement that takes
/// parameters.
constexpr std::int32_t not_one_statement = -1;
constexpr std::int32_t holds_nul = -2;
constexpr std::int32_t takes_parameters = -3;

/// How long a connection waits for another that holds the database, in milliseconds.
constexpr int busy_wait = 5000;

sqlite3* connection(void* database) {
    return static_cast<sqlite3*>(database);
}

sqlite3_stmt* prepared(void* statement) {
    return static_cast<sqlite3_stmt*>(statement);
}

const char* text_of(const fb_string_t* text) {
    return reinterpret_cast<const char*>(text + 1);
}

std::size_t size_of(const fb_string_t* text) {
    return static_cast<std::size_t>(text->length);
}

bool has_nul(const fb_string_t* text) {
    return std::memchr(text_of(text), 0, size_of(text)) != nullptr;
}

/// The next statement in the SQL from `at` to `end`, prepared into `statement`, and SQLite's
/// code, with `at` moved past it. `statement` stays null when what's left is only white space,
/// comments, and semicolons. SQLite reads at most INT_MAX bytes of it, and says that SQL longer
/// than its limit, a billion bytes, is too big.
int next_statement(sqlite3* database, const char*& at, const char* end, sqlite3_stmt*& statement) {
    statement = nullptr;
    while (at < end) {
        const auto readable = static_cast<int>(std::min<std::ptrdiff_t>(end - at, INT_MAX));
        const char* rest = nullptr;
        const int code = sqlite3_prepare_v2(database, at, readable, &statement, &rest);
        if (code != SQLITE_OK || statement != nullptr) {
            at = rest != nullptr ? rest : end;
            return code;
        }

        if (rest == nullptr || rest <= at) {
            break;
        }

        at = rest;
    }

    return SQLITE_OK;
}

} // namespace

extern "C" {

std::int32_t fb_db_open(const fb_string_t* path, void** database) {
    *database = nullptr;
    if (has_nul(path)) {
        return holds_nul;
    }

    const std::string name(text_of(path), size_of(path));
    sqlite3* opened = nullptr;
    const int code =
        sqlite3_open_v2(name.c_str(), &opened,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, nullptr);
    if (opened == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    sqlite3_busy_timeout(opened, busy_wait);
    *database = opened;
    return code;
}

fb_string_t* fb_db_message(void* database) {
    return fb_string_from_cstring(sqlite3_errmsg(connection(database)));
}

void fb_db_close(void* database) {
    // Its statements go with it, so it closes now, not once they're let go of.
    while (sqlite3_stmt* statement = sqlite3_next_stmt(connection(database), nullptr)) {
        sqlite3_finalize(statement);
    }

    sqlite3_close_v2(connection(database));
}

std::int32_t fb_db_run(void* database, const fb_string_t* sql) {
    if (has_nul(sql)) {
        return holds_nul;
    }

    const char* at = text_of(sql);
    const char* const end = at + size_of(sql);
    while (true) {
        sqlite3_stmt* statement = nullptr;
        int code = next_statement(connection(database), at, end, statement);
        if (code != SQLITE_OK || statement == nullptr) {
            return code;
        }

        if (sqlite3_bind_parameter_count(statement) != 0) {
            sqlite3_finalize(statement);
            return takes_parameters;
        }

        do {
            code = sqlite3_step(statement);
        } while (code == SQLITE_ROW);
        sqlite3_finalize(statement);
        if (code != SQLITE_DONE) {
            return code;
        }
    }
}

std::int32_t fb_db_prepare(void* database, const fb_string_t* sql, void** statement) {
    *statement = nullptr;
    if (has_nul(sql)) {
        return holds_nul;
    }

    const char* at = text_of(sql);
    const char* const end = at + size_of(sql);
    sqlite3_stmt* first = nullptr;
    const int code = next_statement(connection(database), at, end, first);
    if (code != SQLITE_OK) {
        return code;
    }

    if (first == nullptr) {
        return not_one_statement;
    }

    // What follows has to be nothing, though it needn't prepare: any statement there, or SQL
    // that isn't one, would be left out.
    sqlite3_stmt* second = nullptr;
    if (next_statement(connection(database), at, end, second) != SQLITE_OK || second != nullptr) {
        sqlite3_finalize(second);
        sqlite3_finalize(first);
        return not_one_statement;
    }

    *statement = first;
    return SQLITE_OK;
}

void fb_db_finalize(void* statement) {
    sqlite3_finalize(prepared(statement));
}

std::int32_t fb_db_parameter_count(void* statement) {
    return sqlite3_bind_parameter_count(prepared(statement));
}

std::int32_t fb_db_parameter_index(void* statement, const fb_string_t* name) {
    if (has_nul(name)) {
        return 0;
    }

    const std::string text(text_of(name), size_of(name));
    return sqlite3_bind_parameter_index(prepared(statement), text.c_str());
}

std::int32_t fb_db_bind_null(void* statement, const std::int32_t index) {
    return sqlite3_bind_null(prepared(statement), index);
}

std::int32_t fb_db_bind_int(void* statement, const std::int32_t index, const std::int64_t value) {
    return sqlite3_bind_int64(prepared(statement), index, value);
}

std::int32_t fb_db_bind_real(void* statement, const std::int32_t index, const double value) {
    return sqlite3_bind_double(prepared(statement), index, value);
}

std::int32_t fb_db_bind_text(void* statement, const std::int32_t index, const fb_string_t* value) {
    return sqlite3_bind_text64(prepared(statement), index, text_of(value), size_of(value),
                               SQLITE_TRANSIENT, SQLITE_UTF8);
}

std::int32_t fb_db_bind_blob(void* statement, const std::int32_t index, const fb_array_t* value) {
    // A null pointer would bind NULL, and an empty array may have none.
    if (value->count == 0) {
        return sqlite3_bind_zeroblob(prepared(statement), index, 0);
    }

    return sqlite3_bind_blob64(prepared(statement), index, value->data,
                               static_cast<sqlite3_uint64>(value->count), SQLITE_TRANSIENT);
}

void fb_db_clear(void* statement) {
    sqlite3_clear_bindings(prepared(statement));
}

std::int32_t fb_db_step(void* statement) {
    return sqlite3_step(prepared(statement));
}

void fb_db_reset(void* statement) {
    sqlite3_reset(prepared(statement));
}

std::int32_t fb_db_column_count(void* statement) {
    return sqlite3_column_count(prepared(statement));
}

fb_string_t* fb_db_column_name(void* statement, const std::int32_t index) {
    const char* name = sqlite3_column_name(prepared(statement), index);
    if (name == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    return fb_string_from_cstring(name);
}

std::int32_t fb_db_column_type(void* statement, const std::int32_t index) {
    return sqlite3_column_type(prepared(statement), index);
}

std::int64_t fb_db_column_int(void* statement, const std::int32_t index) {
    return sqlite3_column_int64(prepared(statement), index);
}

double fb_db_column_real(void* statement, const std::int32_t index) {
    return sqlite3_column_double(prepared(statement), index);
}

fb_string_t* fb_db_column_text(void* statement, const std::int32_t index) {
    // The text first, then its size, which SQLite says is the order that converts once.
    const unsigned char* text = sqlite3_column_text(prepared(statement), index);
    const int size = sqlite3_column_bytes(prepared(statement), index);
    if (text == nullptr) {
        return fb_string_from_bytes("", 0);
    }

    return fb_string_from_bytes(reinterpret_cast<const char*>(text), size);
}

void fb_db_column_blob(void* statement, const std::int32_t index, fb_array_t* output) {
    const void* blob = sqlite3_column_blob(prepared(statement), index);
    const int size = sqlite3_column_bytes(prepared(statement), index);
    output->count = 0;
    if (blob == nullptr || size == 0) {
        return;
    }

    fb_array_reserve(output, 1, size, nullptr);
    std::memcpy(output->data, blob, static_cast<std::size_t>(size));
    output->count = size;
}

std::int64_t fb_db_changes(void* database) {
    return sqlite3_changes64(connection(database));
}

std::int64_t fb_db_last_id(void* database) {
    return sqlite3_last_insert_rowid(connection(database));
}

std::int32_t fb_db_in_transaction(void* database) {
    return sqlite3_get_autocommit(connection(database)) == 0 ? 1 : 0;
}
}
