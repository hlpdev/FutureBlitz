# The review of M6 step 23, std.uuid and std.db over SQLite, all but Query<T> (it waits on step 17's Field-by-Field fill) and Uuid.V7 (it waits on step 16's clock)

12 confirmed findings: 0 high, 6 medium, 6 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S23-sqlite-1 (medium): Database.Rows(sql) with no values binds every parameter to NULL silently, where Execute(sql) panics

Status: fixed in b8fcfb9 (Database.Rows(sql) calls Expect(0) on its Statement, so it panics at the caller; panic_db_rows_parameters)

Where: std/db/database.fb:435

Database.Rows(sql) calls Prepare(sql).Rows(), which binds nothing and never checks the parameter count. So SQL that takes a parameter runs with it as NULL and quietly gives no rows. Execute(sql) panics in the same case, for the reason panic_db_execute_parameters.fb gives ("which would otherwise be NULL"). Rows' own comment says its parameters are "bound to values as Execute's are", and docs/stdlib.md says "A wrong number of values ... panic at the caller". Zero values for one parameter is a wrong number. Verified in debug and release.

```
Import std.core
Import std.db

Function Run() Throws DatabaseError
    Local db = Try Database.Open(":memory:")
    Try db.Execute("CREATE TABLE t (name TEXT)")
    Try db.Execute("INSERT INTO t VALUES ('a')")
    Local rows = Try db.Rows("SELECT * FROM t WHERE name = ?")
    Print("rows: {rows.Count}")
End Function

Try
    Run()
Catch e As DatabaseError
    Print("failed: {e.Message}")
End Try
```

Expected: panic: a value is given for each of the statement's parameters, 1, not 0 at <file>:8:<col>  (a panic at the caller, as Execute(sql) and Rows(sql, a, b) with too few values give)

Actual: rows: 0

Suggested fix: In Database.Rows(sql), prepare the statement, call Expect(0) on it, then Collect, so Rows(sql) panics at the caller like the overloads that take values. Add a panic_db_rows_parameters.fb test.

Skeptic: I ran C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s23/sqlite-v/f1.fb (the reviewer's program). It printed "rows: 0" with exit 0 in both debug and release. My control f1b.fb calls db.Rows(sql with 2 '?', "a") and panics, in debug and release: "a value is given for each of the statement's parameters, 2, not 1 at f1b.fb:8:22". The cause is std/db/database.fb:435-437, where Database.Rows(sql) returns Prepare(sql).Rows(). Statement.Rows() (793) goes straight to Collect with no Expect(0). Every overload that takes values calls Expect(n), and Execute(sql) panics through TAKES_PARAMETERS. docs/stdlib.md says Rows(sql, values...) binds parameters "one each" and that "A wrong number of values ... panic at the caller". Zero values for a one-parameter query is a wrong number, and here the parameter silently becomes NULL.

## S23-sqlite-2 (medium): A Statement's Rows reads column names before stepping, so after a schema change it drops, keeps, or makes up columns

Status: fixed in 4b401ed (Collect reads the column count and names after the first step that gives a row; db_statements reuses a Statement across ADD, RENAME, and DROP COLUMN)

Where: std/db/database.fb:948

Collect reads fb_db_column_count and fb_db_column_name before the first fb_db_step. When the schema has changed since the last run, as after ALTER TABLE (a migration, or another connection), sqlite3_step re-prepares the statement and its columns change. The names and count read earlier are stale. A column that was added is missing from every Row. A dropped column still shows, as a NULL under its old name: fb_db_column_type is called past the real column count, which db.hpp calls a bug. A renamed column keeps its old name. Database.Rows always prepares fresh SQL, so only a reused Statement hits this. Verified in debug and release.

```
Import std.core
Import std.db

Function Run() Throws DatabaseError
    Local db = Try Database.Open(":memory:")
    Try db.Execute("CREATE TABLE t (a INTEGER, b TEXT)")
    Try db.Execute("INSERT INTO t VALUES (1, 'x')")
    Local all = Try db.Prepare("SELECT * FROM t")
    Print((Try all.Rows())[0].Count)
    Try db.Execute("ALTER TABLE t ADD COLUMN c TEXT DEFAULT 'see'")
    Local added = (Try all.Rows())[0]
    Print(added.Count, (Try db.Rows("SELECT * FROM t"))[0].Count)
    Try db.Execute("ALTER TABLE t RENAME COLUMN a TO renamed")
    Try db.Execute("ALTER TABLE t DROP COLUMN b")
    Try db.Execute("ALTER TABLE t DROP COLUMN c")
    Local row = (Try all.Rows())[0]
    Print(row.Count, row.Name(0), row.Type(row.Count - 1))
    Print(row.Integer("renamed") ?? -1)
End Function

Try
    Run()
Catch e As DatabaseError
    Print("failed: {e.Message}")
End Try
```

Expected: 2
3 3
1 renamed INTEGER
1

Actual: 2
2 3
3 a NULL
panic: the row has no column named 'renamed' at f2.fb:18:11

Suggested fix: In Collect, read the column count and names after the first fb_db_step, whether it gave ROW or DONE, since the step re-prepares the statement. Add a test that reuses a Statement across ALTER TABLE ADD, RENAME, and DROP COLUMN.

Skeptic: I ran f2.fb (the reviewer's program) in debug and release. Both printed "2 / 2 3 / 3 a NULL / panic: the row has no column named 'renamed' at f2.fb:18:11". After ALTER TABLE ADD COLUMN, the reused Statement gives rows of 2 columns while a fresh Rows gives 3. After RENAME and DROP it still reports 3 columns, with the old name 'a' and a NULL past the real column count. In Collect (database.fb:948-966), fb_db_column_count and fb_db_column_name are read before the first fb_db_step, and sqlite3_step re-prepares the statement after a schema change. Later fb_db_column_type is called at indexes past the new count. db.hpp:12 says "std.db checks indexes and counts first, so a bad one here is a bug".

## S23-sqlite-3 (medium): Close doesn't close the connection while a Statement lives: the file and its locks stay held

Status: fixed in 508ff97 (fb_db_close finalizes the connection's statements, through sqlite3_next_stmt, before it closes; a Statement keeps its parameter count, and its Destroy skips a closed Database's; the CLI test's EXCLUSIVE-lock case)

Where: std/db/database.fb:314

Database.Close says it "Closes it now", and docs/stdlib.md says "Close() closes it early". But fb_db_close is sqlite3_close_v2, which leaves a zombie connection while any statement is unfinalized. A Statement keeps its handle until its own last reference goes. Until then the file stays open, so on Windows it can't be deleted or replaced (remove() gave -1 after Close and 0 once the Statement was let go of). Any lock the connection holds also stays. Below, with locking_mode EXCLUSIVE, another connection to the closed database waits 5 seconds and then gets BUSY. With the Prepare line removed, the same program prints 1. IsClosed reports True throughout.

```
Import std.core
Import std.db

Function Run() Throws DatabaseError
    Local first = Try Database.Open("locked.db")
    Try first.Execute("PRAGMA locking_mode = EXCLUSIVE; CREATE TABLE t (n INTEGER)")
    Local statement = Try first.Prepare("SELECT count(*) FROM t")
    first.Close()
    Print(first.IsClosed)
    Local second = Try Database.Open("locked.db")
    Try second.Execute("INSERT INTO t VALUES (1)")
    Print((Try second.Rows("SELECT count(*) FROM t"))[0].Integer(0) ?? -1)
End Function

Try
    Run()
Catch e As DatabaseError
    Print("{e.Kind}: {e.Message}")
End Try
```

Expected: True
1

Actual: True
BUSY: database is locked   (after the 5-second busy wait)

Suggested fix: Have Close finalize the Database's open Statements, for example by tracking them (Weak references or handles) and setting each one's Handle to Null so its Destroy, Parameters, and Clear skip it. Then the connection really closes and later use panics as documented. Otherwise, document that Close leaves the file open until its Statements are let go of. Add a test either way.

Skeptic: I ran f3.fb (the reviewer's EXCLUSIVE-lock program). In debug and release it printed "True" and then, after the 5-second wait (8 s total), "BUSY: database is locked". f3b.fb, the same program without the Prepare line, printed "True / 1". My f3c.fb calls C's remove() right after Close. With a live Statement, Close reported IsClosed True and remove gave -1. It gave 0 once the Statement went, and 0 with no Statement at all, in debug and release. The cause is fb_db_close (db.cpp:102-104), which is sqlite3_close_v2 and leaves a zombie connection while a statement is unfinalized. Statement only finalizes in its own Destroy (database.fb:627). This contradicts Close's doc "Closes it now" (database.fb:312) and stdlib.md's "Close() closes it early".

## S23-tx-1 (medium): A Statement's Rows reads its column names before stepping, so a statement SQLite re-prepares after a schema change gives values under the wrong names

Status: fixed in 4b401ed (the same fix as S23-sqlite-2)

Where: std/db/database.fb:951

Statement.Collect builds `names` from fb_db_column_count and fb_db_column_name before the first fb_db_step. When the schema changes after Prepare (an ALTER TABLE on this connection, or on another connection, such as a second process running Migrate), sqlite3_step quietly re-prepares the statement and the column count and names change. Collect still uses the old names and the old count, and reads cells by the old indexes. Dropping a column makes the row put the next column's value under the dropped column's name and read NULL for the real one. Adding a column leaves the new column out. The values are wrong and nothing panics or throws. The same happens through another connection (checked with two Databases on one file).

```
// A Statement prepared before a column it reads is dropped: SQLite prepares it again as it runs.
Import std.core
Import std.db

Try
    Local db = Database.Open(":memory:")
    db.Execute("CREATE TABLE t (a INTEGER, b INTEGER); INSERT INTO t VALUES (1, 2)")
    Local query = db.Prepare("SELECT * FROM t")
    Local before = query.Rows()[0]
    Print(before.Count, before.Name(0), before.Integer(0) ?? -1, before.Name(1), before.Integer(1) ?? -1)
    db.Execute("ALTER TABLE t DROP COLUMN a")
    Local row = query.Rows()[0]
    Print(row.Count, row.Name(0), row.Integer(0) ?? -1)
    Print(row.Integer("b") ?? -1)
Catch e As DatabaseError
    Print(e.Message)
End Try
```

Expected: 2 a 1 b 2
1 b 2
2

Actual: 2 a 1 b 2
2 a 2
-1
(the same in debug and release; with ADD COLUMN instead, Count stays 1 where it should be 2)

Suggested fix: In Collect, read the column count and names after the first fb_db_step, which is when SQLite re-prepares. Step first, build `names` from the statement as it is now, then read cells. When the first step is DONE, the names can be read then, or skipped. Add a db_statements case that drops and adds a column under a held Statement.

Skeptic: I ran the finding's program as tx1.fb, debug and release. Both print `2 a 1 b 2` / `2 a 2` / `-1`. After DROP COLUMN a, the held Statement's row still has Count 2 and the name 'a', with b's value 2 under it, and Integer("b") reads NULL. An ADD COLUMN variant (tx1b.fb: one column a, then ADD COLUMN b DEFAULT 7) prints `1 a 1` both times, so the new column is left out. Nothing panics or throws. Cause: Statement.Collect (database.fb:951-954) builds `names` from fb_db_column_count and fb_db_column_name before the first fb_db_step. SQLite re-prepares on that step after a schema change, and Collect keeps reading cells by the old count (line 961). The values come back under the wrong names, silently.

## S23-tx-2 (medium): Database.Rows(sql) with no values runs SQL that takes parameters with them all NULL instead of panicking

Status: fixed in b8fcfb9 (the same fix as S23-sqlite-1)

Where: std/db/database.fb:435

Database.Rows(sql) calls Prepare(sql).Rows(), which is the Statement's 'as they're bound' form and never calls Expect. A freshly prepared statement's parameters are NULL, so `db.Rows("... WHERE id = ?")` with the value left out quietly matches nothing. docs/stdlib.md says Rows(sql, values...) binds one value to each parameter and that 'A wrong number of values ... panic at the caller'. Execute(sql) panics in this case (panic_db_execute_parameters, whose comment says the parameters 'would otherwise be NULL'), and Rows(sql, a) panics for one value too few or too many. Only the zero-value Rows lets it through.

```
// Database.Rows(sql) with no values on SQL that takes a parameter.
Import std.core
Import std.db

Try
    Local db = Database.Open(":memory:")
    db.Execute("CREATE TABLE t (id INTEGER, name TEXT); INSERT INTO t VALUES (1, 'a')")
    Local rows = db.Rows("SELECT name FROM t WHERE id = ?")
    Print("rows: {rows.Count}")
Catch e As DatabaseError
    Print(e.Message)
End Try
```

Expected: panic: a value is given for each of the statement's parameters, 1, not 0 at <file>.fb:8:5 (or Execute(sql)'s panic that it binds no parameters)

Actual: rows: 0 (debug and release)

Suggested fix: In Database.Rows(sql), prepare the statement, call statement.Expect(0) (it's @PanicsAtCaller, so the panic lands on the caller's line), then return statement.Rows(). Add a panic_db_rows_parameters program.

Skeptic: tx2.fb (the finding's program) prints `rows: 0` in debug and release, with no panic. Worse, tx2b.fb runs `db.Rows("UPDATE t SET name = ?")` with no value. It prints `True` for IsNull on the row it read back, in debug and release, so every name was quietly set to NULL. Database.Rows(sql) (database.fb:435) returns Prepare(sql).Rows(), the 'as they're bound' form, which never calls Expect. stdlib.md says Rows(sql, values...) binds 'one each' and that 'A wrong number of values ... panic at the caller'. Execute(sql) panics in exactly this case (lines 345-347), and Rows(sql, a) panics on a wrong count. Only the zero-value Rows lets parameterized SQL run with NULLs.

## S23-uuid-1 (medium): Database.Rows(sql) with no values silently binds NULL to a statement's parameters instead of panicking

Status: fixed in b8fcfb9 (the same fix as S23-sqlite-1)

Where: std/db/database.fb:435

docs/stdlib.md (lines 541-542, 563) says Execute(sql, values...) and Rows(sql, values...) bind each parameter to one value, and that a wrong number of values panics at the caller. Rows(sql)'s own comment says its parameters are bound 'as Execute's are', and Execute(sql) panics when its SQL takes parameters. But the zero-value Database.Rows(sql) calls Prepare(sql).Rows(), which goes straight to Collect() with no Expect(0) and no parameter check. So a query whose value was left out runs with every parameter set to NULL. A query like `WHERE id = ?` then quietly gives no rows, and the program gets no panic. Debug and release behave the same.

```
Import std.core
Import std.db

Function Run() Throws DatabaseError
    Local db = Try Database.Open(":memory:")
    Try db.Execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)")
    Try db.Execute("INSERT INTO users (id, name) VALUES (?, ?)", 1, "ada")
    // The value for ? is left out: a wrong number of values, 0 for 1.
    Local rows = Try db.Rows("SELECT name FROM users WHERE id = ?")
    Print(rows.Count)
End Function

Try
    Run()
Catch e As DatabaseError
    Print("failed: {e.Message}")
End Try
```

Expected: A panic at the Rows call (h.fb:9), the same as Execute(sql) gives for SQL that takes parameters ("... binds no parameters, and a statement in its SQL takes some") or as Expect gives ("a value is given for each of the statement's parameters, 1, not 0").

Actual: 0 (in debug and release; the ? is bound to NULL and the query matches nothing)

Suggested fix: In Database.Rows(sql), prepare the statement and panic when its Parameters isn't 0, as Execute(sql) does for TAKES_PARAMETERS. Statement.Rows() can keep running 'as they're bound'. Add a panic_db_rows_parameters program test.

Skeptic: I ran the finding's h.fb under .claude/worktrees/rev-scratch/s23/uuidref/. It printed 0 and exited 0 in both debug and release, with no panic. The code matches: database.fb:435-437 Database.Rows(sql) is just `Return Try Prepare(sql).Rows()`, and Statement.Rows() (793-796) goes straight to Collect() with no Expect and no parameter check. So a `?` left without a value runs as NULL. Database.Rows(sql) prepares a fresh statement, so the caller has no way to bind it, and the parameter is always NULL. This breaks three things. stdlib.md says Rows(sql, values...) binds its parameters to its values 'one each', and that 'A wrong number of values ... panic at the caller'. The method's own comment says it binds 'as Execute's are', and Execute(sql) panics with TAKES_PARAMETERS (database.fb:345-347; I ran j.fb and got 'panic: Execute(sql) binds no parameters...' at j.fb:6:9). The lane's own test, tests/programs/panic_db_execute_parameters.fb, gives the reason for that panic: parameters 'which would otherwise be NULL'. Database.Rows(sql) falls into exactly that case without panicking. Nothing in tests/programs relies on the NULL binding: every zero-value db.Rows call in db_*.fb has SQL with no parameters.

## S23-sqlite-4 (low): A NaN Float32 or Float64 binds as NULL, not as a real

Status: fixed in 9e8d3bc (Float64's BindTo, and Float32's through it, throw a DatabaseError of OTHER for a NaN; db_errors)

Where: std/db/database.fb:128

SqlValue says a Float32 or Float64 binds "as a real", and NULL is meant to come only from Nullable(value) with a Null value. But sqlite3_bind_double stores NaN as NULL. A non-null NaN therefore comes back as Null from Row.Real, with ColumnType NULL. Inserting it into a REAL NOT NULL column throws a CONSTRAINT error, though the program passed a value. Infinities round-trip correctly. Verified in debug and release.

```
Import std.core
Import std.db

Function Run() Throws DatabaseError
    Local db = Try Database.Open(":memory:")
    Local zero = 0.0 As Float64
    Local nan = zero / zero
    Local row = (Try db.Rows("SELECT ?, typeof(?1)", nan))[0]
    Print(row.Type(0), row.Text(1) ?? "", row.Real(0) == Null)
    Try db.Execute("CREATE TABLE readings (value REAL NOT NULL)")
    Try db.Execute("INSERT INTO readings VALUES (?)", nan)
    Print("stored")
End Function

Try
    Run()
Catch e As DatabaseError
    Print("{e.Kind}: {e.Message}")
End Try
```

Expected: Either a DatabaseError for the NaN when it's bound (as UInt64 past Int64's largest throws), or docs on SqlValue and in docs/stdlib.md saying a NaN binds as NULL. Not a silent REAL-to-NULL change.

Actual: NULL null True
CONSTRAINT: NOT NULL constraint failed: readings.value

Suggested fix: In Float32's and Float64's BindTo, throw a DatabaseError (OTHER, like UInt64's: "SQLite can't hold a NaN") when Self is NaN. Or document that NaN binds as NULL. Add a NaN case to db_errors.fb or db_execute.fb.

Skeptic: I ran f4.fb in debug and release; I added a Float32 NaN to the reviewer's program. Both printed "NULL null True / NULL null / CONSTRAINT: NOT NULL constraint failed: readings.value". Float32 and Float64 BindTo (database.fb:122-132) pass NaN straight to sqlite3_bind_double, which stores it as NULL. SqlValue's doc (database.fb:53-54) says a Float "as a real", and stdlib.md says NULL comes from Nullable with a Null value. The value is silently changed to NULL. UInt64 throws for a value SQLite can't hold, which is the precedent for throwing here. I rated it low rather than medium: NaN is an edge value, and this is SQLite's own documented behavior, so a doc line or a throw closes it.

## S23-sqlite-5 (low): No test opens a file database, and the CORRUPT kind and the 5-second busy wait are untested

Status: fixed in 508ff97 (the CLI test "a program's database is a file it makes, opens again, and shares" opens a file, reopens it with mode=ro, waits out a write lock to BUSY, at least 4 seconds, and opens a file that isn't a database, CORRUPT; it's a CLI test since a program test has no folder of its own to write in, and its files' paths are whole)

Where: tests/programs/db_errors.fb:1

Every test database is ":memory:" or a shared-cache in-memory URI. No test covers the main use: Open on a file path, made when there's none, keeping what's written, and reopened, including the documented "file:data.db?mode=ro". DatabaseError's CORRUPT kind (SQLite's 11 and 26) is never produced by a test. BUSY is tested only through a shared cache's SQLITE_LOCKED (6), which doesn't wait. So nothing checks SQLite's own BUSY (5) or the 5-second busy timeout that fb_db_open sets: removing sqlite3_busy_timeout would leave every test passing. The behavior works when tried (program below), but the tests don't check it.

```
Import std.core
Import std.db

Function Report(e: DatabaseError)
    Print("{e.Kind}: {e.Message}")
End Function

// Run beside a file notdb.db holding over 100 bytes of plain text.
Function Run() Throws DatabaseError
    Local made = Try Database.Open("kinds.db")
    Try made.Execute("CREATE TABLE t (n INTEGER)")
    Try made.Execute("INSERT INTO t VALUES (?)", 1)
    made.Close()
    Local again = Try Database.Open("file:kinds.db?mode=ro")
    Print((Try again.Rows("SELECT n FROM t"))[0].Integer(0) ?? -1)
    Try
        again.Execute("INSERT INTO t VALUES (2)")
    Catch e As DatabaseError
        Report(e)
    End Try

    Local first = Try Database.Open("kinds.db")
    Local second = Try Database.Open("kinds.db")
    Local transaction = Try first.Begin()
    Try first.Execute("INSERT INTO t VALUES (3)")
    Try
        second.Execute("INSERT INTO t VALUES (4)")
    Catch e As DatabaseError
        Report(e)
    End Try
    Try transaction.Commit()

    Try
        (Try Database.Open("notdb.db")).Rows("SELECT * FROM sqlite_master")
    Catch e As DatabaseError
        Report(e)
    End Try
End Function

Try
    Run()
Catch e As DatabaseError
    Report(e)
End Try
```

Expected: A test whose output checks: 1 / OTHER: attempt to write a readonly database / BUSY: database is locked (after about 5 s) / CORRUPT: file is not a database

Actual: The program prints those lines (BUSY after about 5 s), but no test in tests/programs or tests/cli covers a file database, CORRUPT, or SQLite's BUSY with its wait.

Suggested fix: Add a db_files.fb test, or a part of db_errors.fb, that writes into the test's temp folder: open a file, reopen it, open it with mode=ro, a write lock from a second connection (BUSY), and a non-database file (CORRUPT). Keep the busy case to one wait, or lower it for the test if the wait time ever becomes settable.

Skeptic: I grepped tests/programs and tests/cli/cli_tests.cpp. Every Database.Open that succeeds is ":memory:" or a mode=memory&cache=shared URI. The only file paths are failures: "no such folder/data.db" and one holding a NUL. No test produces CORRUPT. The only BUSY outputs (db_errors.out:20, db_transactions.out:16) are "database table is locked", the shared cache's SQLITE_LOCKED, which never runs the busy handler. So removing sqlite3_busy_timeout (db.cpp:93) would leave every test passing. I ran the reviewer's f5.fb, and the behavior works: "1 / OTHER: attempt to write a readonly database / BUSY: database is locked / CORRUPT: file is not a database", 8 s total. So this is a test gap for documented behavior (file open and reopen, mode=ro, SQLite's BUSY with its 5 s wait, CORRUPT), not wrong behavior.

## S23-tx-3 (low): Migrate doesn't throw when the version it reads again inside its Transaction is past the last migration

Status: fixed in 10f9acf (Version(count) throws past the last migration, read before the loop and again inside each Transaction; the CLI test's migrate section)

Where: std/db/database.fb:565

Migrate checks `version > migrations.Count` only on the version it reads before beginning. Inside each migration's Transaction it reads the version again, because 'Another connection may have run it while this one waited to begin', but it only compares that version with `number`. Suppose a newer program migrates the file past this program's last migration while this one waits at BEGIN IMMEDIATE. Then every migration is skipped and Migrate returns normally, though the database is now newer than the program. Started a moment later, the same program throws OTHER. The outcome depends on timing, and docs/stdlib.md says 'a database past the program's last throws'. No test covers the concurrent path (the re-read itself works: the same setup with the other connection migrating only to version 1 runs the migration once).

```
// A newer program migrates the file past this one's last migration while this one's Migrate waits
// to begin.
Import std.core
Import std.db

Const first = "CREATE TABLE log (step TEXT); INSERT INTO log VALUES ('one')"
Const second = "INSERT INTO log VALUES ('two')"
Const ready = Channel<Int>(1)

Function Older(): String
    Try
        Local db = Database.Open("mig2.db")
        ready.Send(1)
        db.Migrate(first)
        Return "older migrated"
    Catch e As DatabaseError
        Return "older: {e.Kind} {e.Message}"
    End Try
End Function

Try
    Local db = Database.Open("mig2.db")
    Local holding = db.Begin()
    Local older = Spawn(Function() => Older())
    ready.Receive()
    // Long enough for the older one to read version 0 and wait to begin.
    db.Rows("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 3000000) " +
            "SELECT count(*) FROM c")
    db.Execute(first)
    db.Execute(second)
    db.Execute("PRAGMA user_version = 2")
    holding.Commit()
    Print(older.Wait())
Catch e As DatabaseError
    Print("failed:", e.Kind, e.Message)
End Try
```

Expected: older: OTHER the database is at version 2, and there are 1 migrations (what the same program prints when the Spawn comes after holding.Commit())

Actual: older migrated (debug and release, every run; delete mig2.db between runs)

Suggested fix: Inside the loop, after Begin, read the version once. If it's past migrations.Count, throw the same OTHER error (the Transaction rolls back as it's let go). Otherwise run the migration only when the version is below number. Add a test like this program (and the version-1 variant) for the concurrent path.

Skeptic: I ran the finding's program as tx3.fb, deleting mig2.db first each time: 3 debug runs and 2 release runs. Every run printed `older migrated` / `version 2`. The older Migrate must have read version 0 before it began; otherwise it would have thrown, and its migration's CREATE TABLE log would have failed had it run. Inside the Transaction it read 2, skipped, and committed. A sequential variant (tx3seq.fb: the newer one commits version 2 first, then the older one starts) prints `older: OTHER the database is at version 2, and there are 1 migrations`. Cause: Migrate (database.fb:557) checks `version > migrations.Count` only before the loop, and the re-read inside the Transaction (line 565) is compared only with `number`. stdlib.md says 'a database past the program's last throws', but whether it does here depends on timing. The window is narrow, hence low.

## S23-tx-4 (low): A Float64 NaN binds as NULL, though the docs say a Float binds as a real

Status: fixed in 9e8d3bc (the same fix as S23-sqlite-4)

Where: std/db/database.fb:128

Float32's and Float64's BindTo pass the value to sqlite3_bind_double, which stores NaN as NULL. A bound NaN reads back as ColumnType.NULL, and inserting it into a REAL NOT NULL column fails with a confusing 'NOT NULL constraint failed'. Neither the SqlValue comment ('a Float32 or Float64 as a real') nor docs/stdlib.md mentions this.

```
// A Float64 NaN bound to a parameter.
Import std.core
Import std.math
Import std.db

Try
    Local db = Database.Open(":memory:")
    db.Execute("CREATE TABLE t (x REAL NOT NULL)")
    Local nan = NaN As Float64
    Print(db.Rows("SELECT ?", nan)[0].Type(0))
    db.Execute("INSERT INTO t VALUES (?)", nan)
    Print("inserted")
Catch e As DatabaseError
    Print(e.Kind, e.Message)
End Try
```

Expected: As documented, REAL then inserted. SQLite can't keep NaN, so either the docs and comment say NaN binds NULL, or BindTo rejects it with an error that names NaN.

Actual: NULL
CONSTRAINT NOT NULL constraint failed: t.x

Suggested fix: Pick one: say in the SqlValue comment and docs/stdlib.md that NaN binds as NULL, or have the Float BindTos throw a DatabaseError (OTHER) for NaN, as UInt64's does for values SQLite can't hold. Add a case to the tests either way.

Skeptic: tx4.fb (the finding's program) prints `NULL` / `CONSTRAINT NOT NULL constraint failed: t.x` in debug and release. Float32's and Float64's BindTo (database.fb:122-132) pass NaN to fb_db_bind_real, and SQLite stores it as NULL. The SqlValue comment (line 53) says 'a Float32 or Float64 as a real'. So a bound Float64 silently changes to a NULL, reads back as Null, and fails a REAL NOT NULL column with a misleading NOT NULL message. UInt64's BindTo throws for a value SQLite can't hold, but NaN is passed through. stdlib.md itself doesn't say 'real', so this is the comment's claim and a surprising silent change rather than a spec break. Hence low.

## S23-tx-5 (low): A Statement's Clear and Parameters don't panic once its Database is closed, though Bind, Execute, and Rows do

Status: fixed in 508ff97 (Clear and Expect check Owner.Connection() first, so the values-taking Execute and Rows panic before binding; Parameters still works, from the count it keeps; panic_db_closed_clear and panic_db_closed_values)

Where: std/db/database.fb:658

The step's call is that using a Database after Close panics, and the Statement follows it in Bind (Owner.Connection()) and in Run and Collect. Clear and the Parameters property skip the check and work on the zombie statement that sqlite3_close_v2 leaves. Execute(a, b) also binds its values before Run's check panics. Nothing crashes, but the closed check is applied unevenly.

```
// A Statement's Clear and Parameters once its Database is closed.
Import std.core
Import std.db

Try
    Local db = Database.Open(":memory:")
    Local statement = db.Prepare("SELECT ?, ?")
    db.Close()
    statement.Clear()
    Print(statement.Parameters)
    statement.Execute(1, 2)
Catch e As DatabaseError
    Print(e.Message)
End Try
```

Expected: panic: the Database is closed at closed_statement.fb:9:5

Actual: 2
panic: the Database is closed at closed_statement.fb:11:5

Suggested fix: Make Clear @PanicsAtCaller and call Owner.Connection() first. Give Parameters the same check, as Database.Changes does. Also call Owner.Connection() at the top of Expect, so the values-taking Execute and Rows panic before binding. Add the case to panic_db_closed or a new panic program.

Skeptic: closed_statement.fb (the finding's program) prints `2` and then `panic: the Database is closed at closed_statement.fb:11:5`, debug and release, so Clear() after Close runs without a panic. tx5b.fb calls `statement.Execute(18446744073709551615 As UInt64)` on a Statement whose Database is closed. In debug and release it prints `threw: OTHER SQLite's integers go up to 9223372036854775807, not 18446744073709551615`: a DatabaseError rather than the panic stdlib.md describes ('using it after that panics'). That's because Execute and Rows with values call Expect and BindTo before Run's and Collect's Owner.Connection() check (lines 671-675, 936-937). Bind checks Owner.Connection() first (lines 639, 651), so the lane meant Statement use after Close to panic, and Clear (line 658) skips the check. One caveat: tests/programs/db_close.fb deliberately prints statement.Parameters after Close and expects 0 (db_close.out line 2). Reading Parameters after Close is therefore intended and is not part of this bug. The confirmed parts are Clear and binding before the closed check.

## S23-uuid-2 (low): docs/stdlib.md's list of std.db panics leaves out two panics the code has

Status: fixed in 1f1fead (stdlib.md names Execute(sql)'s panic at parameters and the outer Commit's panic, and says what Close, Rows(sql), and a NaN do)

Where: docs/stdlib.md:563

docs/stdlib.md's std.db section says only 'A wrong number of values, a column or parameter that isn't there, a wrong type read, and a Transaction ended twice panic at the caller', and line 540 describes Execute(sql) as only 'runs each statement in turn, stopping at the first that fails'. The code panics in two more cases. Transaction.Commit panics while a Transaction begun inside it is open (database.fb:1030-1032, 'a Transaction is committed while one begun inside it is open'). Execute(sql) panics at a statement that takes parameters (database.fb:345-347). Both are calls the lane intended, but std's reference doesn't mention either, so a reader can't learn from the docs that an outer Commit with an inner Transaction still held panics, or that Execute(sql) panics on `?`.

```
Import std.core
Import std.db

Function Run() Throws DatabaseError
    Local db = Try Database.Open(":memory:")
    Local outer = Try db.Begin()
    Local inner = Try db.Begin()
    Try db.Execute("CREATE TABLE t (n INTEGER)")
    Try outer.Commit()
    Print("committed")
End Function

Try
    Run()
Catch e As DatabaseError
    Print("failed: {e.Message}")
End Try
```

Expected: Going by docs/stdlib.md's list of panics, which leaves this case out: no panic ("committed", or a DatabaseError). The docs should say this panics.

Actual: panic: a Transaction is committed while one begun inside it is open at i.fb:9:9 (exit code 101, in debug and release). Separately, `Try db.Execute("SELECT ? AS x")` panics with "Execute(sql) binds no parameters, and a statement in its SQL takes some", which the docs don't mention either.

Suggested fix: Add both to docs/stdlib.md: in the Execute(sql) bullet, that it panics at a statement that takes parameters; and in the panic list (or the Begin bullet), that a Commit while a Transaction begun inside it is open panics.

Skeptic: I ran the finding's i.fb. It gives 'panic: a Transaction is committed while one begun inside it is open at i.fb:9:9', exit 101, in debug and release. I listed every Panic( in std/db/database.fb (lines 208-265, 346, 583, 640-653, 929, 1023, 1031, 1049) against stdlib.md's std.db text. Each maps to something the docs name except two. Close is covered by 'using it after that panics'. The type mismatches are 'a wrong type read'. Column index and name are 'a column ... that isn't there'. Bind's index, name and none are 'a parameter that isn't there'. Expect is 'a wrong number of values', and Ended is 'a Transaction ended twice'. Commit while an inner Transaction is open (line 1031) matches nothing in the list, and the Begin bullet doesn't mention it either. The Execute(sql) parameter panic (line 346) is only loosely covered by 'a wrong number of values', while the Execute(sql) bullet (stdlib.md:540) says only that it runs each statement and stops at the first that fails. The list is otherwise complete, so the gap is real: the docs don't tell a reader about the outer-Commit panic. It is a docs gap only; the behavior is a listed lane decision, which is why I rate it low.
