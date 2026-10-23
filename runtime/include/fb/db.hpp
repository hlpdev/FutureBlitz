#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.db's functions in the runtime, over SQLite, which a program links only when it calls one.
// A database is SQLite's connection and a statement its prepared statement, which std.db holds
// as a Ptr<Void> and gives back to fb_db_close and fb_db_finalize. Strings and arrays are only
// borrowed. What can fail gives SQLite's result code, 0 when it didn't, with the message from
// fb_db_message, or -2 for SQL or a path that holds a NUL, where SQLite would stop reading it.
// std.db checks indexes and counts first, so a bad one here is a bug.
//
// The column types fb_db_column_type gives: 1 an integer, 2 a real, 3 text, 4 a blob, 5 NULL.

extern "C" {

/// Opens the database at `path` into `database`, making it when it isn't there, which
/// fb_db_close gives back, even when opening failed. ":memory:" is a new one in memory, and a URI
/// file name, like "file:shared?mode=memory&cache=shared", is one too. A connection waits up to
/// 5 seconds for another one that holds the database before it gives SQLite's busy code.
std::int32_t fb_db_open(const fb_string_t* path, void** database);
/// What went wrong in the last call on `database` that failed, in English.
fb_string_t* fb_db_message(void* database);
/// Closes `database`, once its statements are finalized.
void fb_db_close(void* database);

/// Runs each statement in `sql`, in turn, until one fails, or -3 when one takes parameters, which
/// it doesn't run.
std::int32_t fb_db_run(void* database, const fb_string_t* sql);
/// `sql`'s one statement, prepared, into `statement`, and 0; or SQLite's code when it doesn't
/// prepare, or -1 when `sql` holds no statement or more than one, and `statement` stays null.
std::int32_t fb_db_prepare(void* database, const fb_string_t* sql, void** statement);
void fb_db_finalize(void* statement);

/// How many parameters `statement` has, and where one of a name, like ":name", is: from 1, or
/// 0 when it has none of that name.
std::int32_t fb_db_parameter_count(void* statement);
std::int32_t fb_db_parameter_index(void* statement, const fb_string_t* name);
/// Binds the parameter at `index`, from 1, to a value.
std::int32_t fb_db_bind_null(void* statement, std::int32_t index);
std::int32_t fb_db_bind_int(void* statement, std::int32_t index, std::int64_t value);
std::int32_t fb_db_bind_real(void* statement, std::int32_t index, double value);
std::int32_t fb_db_bind_text(void* statement, std::int32_t index, const fb_string_t* value);
std::int32_t fb_db_bind_blob(void* statement, std::int32_t index, const fb_array_t* value);
/// Sets every parameter to NULL again.
void fb_db_clear(void* statement);

/// Runs `statement` to its next row: 100 when it has one, 101 when it's done, or what failed.
std::int32_t fb_db_step(void* statement);
/// Starts `statement` again from its first row, with its parameters as they are.
void fb_db_reset(void* statement);

/// The row's columns: how many there are, each one's name and type, and its value as an
/// integer, a real, text, or a blob, into `output`, a UInt8[].
std::int32_t fb_db_column_count(void* statement);
fb_string_t* fb_db_column_name(void* statement, std::int32_t index);
std::int32_t fb_db_column_type(void* statement, std::int32_t index);
std::int64_t fb_db_column_int(void* statement, std::int32_t index);
double fb_db_column_real(void* statement, std::int32_t index);
fb_string_t* fb_db_column_text(void* statement, std::int32_t index);
void fb_db_column_blob(void* statement, std::int32_t index, fb_array_t* output);

/// How many rows the last INSERT, UPDATE, or DELETE on `database` changed, and the rowid of
/// the last row an INSERT added.
std::int64_t fb_db_changes(void* database);
std::int64_t fb_db_last_id(void* database);
/// 1 when `database` is in a transaction, or 0 when it isn't, as when SQLite rolled one back.
std::int32_t fb_db_in_transaction(void* database);
}
