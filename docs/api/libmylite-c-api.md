# libmylite C API

`libmylite` is the primary embedded API. It follows SQLite's handle ownership
style while preserving MariaDB SQL behavior, diagnostics, warnings, affected
rows, insert ids, and richer data types.

## Principles

- Open a database by filename.
- Own all handles explicitly.
- Do not require a daemon, socket, server account, password handshake, or
  network connection for local file use.
- Keep MariaDB internals private.
- Expose stable MyLite result codes and MariaDB-compatible diagnostics.
- Make prepared statements reusable and binary safe.
- Keep raw `MYSQL *` access out of the core lifetime model.

## Handles

```c
typedef struct mylite_db mylite_db;
typedef struct mylite_stmt mylite_stmt;
```

`mylite_db` owns one logical embedded MariaDB connection to one `.mylite` file.
Multiple handles for the same file coordinate through a shared file runtime.

`mylite_stmt` owns one prepared statement.

## Result Codes

Primary result codes stay small and stable. MariaDB errno and SQLSTATE remain
available through diagnostics APIs.

```c
typedef enum mylite_result {
  MYLITE_OK = 0,
  MYLITE_ERROR = 1,
  MYLITE_BUSY = 5,
  MYLITE_NOMEM = 7,
  MYLITE_READONLY = 8,
  MYLITE_IOERR = 10,
  MYLITE_CORRUPT = 11,
  MYLITE_NOTFOUND = 12,
  MYLITE_FULL = 13,
  MYLITE_CONSTRAINT = 19,
  MYLITE_MISUSE = 21,
  MYLITE_ROW = 100,
  MYLITE_DONE = 101
} mylite_result;
```

## Opening And Closing

```c
typedef struct mylite_open_config {
  size_t size;
  int profile;
  unsigned busy_timeout_ms;
  int durability;
  const char *temp_directory;
} mylite_open_config;

int mylite_open(const char *filename, mylite_db **out_db);
int mylite_open_v2(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config);
int mylite_close(mylite_db *db);
```

Flags:

```c
#define MYLITE_OPEN_READONLY   0x00000001U
#define MYLITE_OPEN_READWRITE  0x00000002U
#define MYLITE_OPEN_CREATE     0x00000004U
#define MYLITE_OPEN_EXCLUSIVE  0x00000008U
#define MYLITE_OPEN_URI        0x00000010U
```

Profiles:

```c
#define MYLITE_PROFILE_DEFAULT 0
#define MYLITE_PROFILE_STRICT  1
#define MYLITE_PROFILE_COMPAT  2
```

`mylite_open()` is equivalent to read/write create with default configuration.
Creating a local file writes a versioned `.mylite` header and empty catalog
root. Opening an existing local file validates that header and rejects
unsupported or corrupt formats before the embedded runtime starts.
`mylite_open_config.size` makes the struct growable without breaking ABI.

`mylite_close()` returns `MYLITE_BUSY` when statements or dependent resources
still exist. Deferred close can be added separately if a real use case appears.

Initial implementation status: open/close is backed by MariaDB embedded startup
when the `embedded-dev` CMake preset enables it. MyLite passes owned startup
options, ignores ambient option files with `--no-defaults`, creates a temporary
runtime directory for MariaDB bootstrap files, and removes that directory on the
final close. Current storage-engine smoke builds persist schema namespace
records with default character set, collation, and comment options,
table-definition metadata, rows, autoincrement state, supported indexes, and
rollback-journal and transaction-journal publication state in the primary
`.mylite` file. File-backed opens answer schema and table discovery from the
catalog when no transient MariaDB schema directory exists. Direct or prepared
`BEGIN`, `START TRANSACTION`, `COMMIT`, `ROLLBACK`,
transaction restart through repeated `BEGIN` / `START TRANSACTION`, and
supported direct or prepared session `SET autocommit=0/1/DEFAULT` forms and
prepared single-marker session `SET autocommit=?` values,
including `SET` lists that mix one autocommit assignment with ordinary
non-transaction assignments and duplicate supported session autocommit
assignments applied in order with the final value as session state, support
row-DML transactions over routed MyLite tables. Direct or prepared
transaction modifiers support
explicit `START TRANSACTION READ WRITE`, `COMMIT` / `ROLLBACK` `AND CHAIN`,
`AND NO CHAIN`, and `NO RELEASE` forms for the same bounded scope, including
nested statement rollback for covered failed direct and prepared row-DML
statements and transaction-journal recovery after an unclean process exit.
Direct `START TRANSACTION READ ONLY`, `START TRANSACTION READ WRITE`, direct
and prepared `SET TRANSACTION READ ONLY`, `SET TRANSACTION READ WRITE`,
session `SET TRANSACTION READ ONLY` / `READ WRITE`, direct and prepared
session `SET TRANSACTION ISOLATION LEVEL ...`, transaction read-only and
isolation variable assignments including duplicate supported assignment lists
where the final value wins, and session
`SET completion_type=NO_CHAIN/0/DEFAULT/CHAIN/1` forms are accepted for the
current bounded transaction scope through direct and prepared execution,
including duplicate supported `completion_type` assignments where the final
assignment wins. Isolation controls and isolation variables are accepted as
compatibility setup SQL, not as a storage-isolation guarantee. Read-only
transactions reject direct and prepared durable MyLite storage writes while
allowing simple row DML against tracked temporary tables. `CHAIN` makes later
plain direct or prepared `COMMIT` and `ROLLBACK` use the same chained behavior
as explicit `AND CHAIN`, while explicit `AND NO CHAIN` still overrides it.
Direct `SAVEPOINT`,
`ROLLBACK TO [SAVEPOINT]`, and `RELEASE SAVEPOINT` support case-insensitive
simple unquoted and backtick-quoted savepoint names inside active bounded
row-DML transactions. Double-quoted savepoint names are also supported when the
session has `ANSI_QUOTES` enabled. The same savepoint-control statements can be
prepared and reused for file-backed MyLite transactions. Global
autocommit-control statements, direct-execution parameter markers,
expression-valued or global parameterized transaction-control `SET` forms,
bound `DEFAULT` / `RELEASE` transaction-control values,
`WITH CONSISTENT SNAPSHOT` transaction starts, `RELEASE` completion,
`completion_type=RELEASE/2`, global transaction variable assignments,
duplicate `SET TRANSACTION` characteristics, XA, and direct or prepared DDL
inside an active transaction remain unsupported until the storage and catalog
transaction design is broader.
Existing-file opens preserve storage lock conflicts as
`MYLITE_BUSY` before starting the embedded runtime.

## Direct Execution

```c
typedef int (*mylite_exec_callback)(
    void *ctx,
    int column_count,
    char **values,
    char **column_names);

int mylite_exec(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg);
```

`mylite_exec()` is a convenience API for one-shot SQL. Result values are textual
like SQLite's `sqlite3_exec()` callback; production code that needs repeated
execution or binary-safe values uses prepared statements.

If `errmsg` is non-NULL and an error string is returned, the caller releases it
with `mylite_free()`.

Initial implementation status: `mylite_exec()` runs through the embedded
MariaDB connection in `embedded-dev` builds, returns text result rows, preserves
SQL `NULL` values as `NULL` callback entries, populates MariaDB diagnostics on
query failure, and exposes affected rows plus generated insert ids after
representative temporary-table non-result statements. File-backed MyLite
storage-engine builds support catalog schema namespaces for successful direct
`CREATE/DROP DATABASE` and
`CREATE/DROP SCHEMA` statements, including `ALTER DATABASE` / `ALTER SCHEMA`
updates to default character set, collation, and comment options, routed
`CREATE TABLE` metadata, catalog-backed `DROP TABLE` and simple
`RENAME TABLE`, keyless copy `ALTER` rebuilds, and keyless `INSERT` plus
full-scan `SELECT` over persisted MyLite row pages. Basic CHECK constraints on
routed table definitions are enforced by MariaDB before MyLite handler writes,
and those definitions survive close/reopen through the MyLite catalog.
Basic unindexed generated columns use MariaDB's generated-column evaluation,
with virtual values computed from restored row buffers and stored values kept
in normal MyLite row payloads. The public foreign-key subset supports
validated `RESTRICT` / `NO ACTION` `CREATE TABLE`, copy
`ALTER TABLE ... ADD FOREIGN KEY`, and `ALTER TABLE ... DROP FOREIGN KEY`
metadata over durable routed base tables, including basic self-references to
parent rows that already exist and same-row self-references where the child and
parent key prefixes match, representative immediate exact-index row checks,
representative update/delete ordering, and bounded self-referencing, same-row
self-referencing, and non-self `ON DELETE SET NULL` / `ON UPDATE SET NULL` over
simple nullable child columns, plus bounded `ON DELETE CASCADE` over simple
child table shapes, bounded recursive `ON UPDATE CASCADE` over simple acyclic
child table action chains, supported combinations of those actions, and the
bounded same-row update action matrix for `ON UPDATE SET NULL` and
`ON UPDATE CASCADE`. The subset is advertised to MariaDB through the MyLite
handlerton. Broader multi-table matrices and cyclic or full recursive FK action
chains remain unsupported. `SET DEFAULT` FK action clauses are rejected
explicitly because the selected MariaDB/InnoDB base documents them as
unsupported.
Session
`foreign_key_checks=0`
disables supported FK row checks and parent-table truncate checks without
revalidating existing rows when checks are re-enabled. MariaDB-generated FK
child keys follow copy-ALTER cleanup when explicit compatible keys replace them
or when no-longer-needed retained indexes are explicitly dropped. SQL sequence
object/value surfaces are rejected, and the
default embedded profile does not register MariaDB's virtual `SEQUENCE` storage
engine, so magic generated tables such as
`seq_1_to_10` are unavailable outside ordinary catalog-backed user tables.
MariaDB user-statistics
information-schema surfaces and `userstat` system-variable assignments are
rejected as server observability, while ordinary SQL user variables remain
available. MariaDB statement profiling surfaces such as `SHOW PROFILE`,
`SHOW PROFILES`, profiling system-variable assignment, and
`INFORMATION_SCHEMA.PROFILING` are rejected as server observability. MariaDB
optimizer trace surfaces such as optimizer-trace system-variable assignment
and `INFORMATION_SCHEMA.OPTIMIZER_TRACE` are rejected as server diagnostics.
MariaDB static server-information statements such as `SHOW AUTHORS`,
`SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` are rejected as server metadata.
MariaDB foreign-server metadata statements such as `CREATE SERVER` and
`SHOW CREATE SERVER` are rejected as remote-engine/server metadata. MariaDB
external backup statements such as `BACKUP STAGE`, `BACKUP LOCK`, and
`BACKUP UNLOCK` are rejected as datadir backup-tool coordination. MariaDB query
cache administration such as `FLUSH QUERY CACHE`, `RESET QUERY CACHE`, and
query-cache system-variable assignment is rejected while ordinary
`SELECT SQL_CACHE` and `SELECT SQL_NO_CACHE` remain accepted as uncached
compatibility hints. Dynamic plugin loading is disabled in the default
embedded profile: `have_dynamic_loading` reports `NO`, dynamic-loader probes
are cleared from the generated MariaDB configuration, and plugin
install/uninstall SQL is rejected before MariaDB execution while static
built-ins remain available. Zlib-backed compression is disabled in the default
embedded profile: `have_compress` reports `NO`, `COMPRESS()` and
`UNCOMPRESS()` return `NULL`, and compressed column DDL is rejected with
ordinary MyLite/MariaDB diagnostics.
Native InnoDB is not registered in the default embedded profile, but
file-backed storage-engine builds still route application `ENGINE=InnoDB` DDL
to MyLite storage. Native CSV and partition are not registered either; CSV
table files remain unsupported, and partition DDL continues to fail before
MariaDB execution until MyLite has partition metadata and routing.

## Prepared Statements

```c
int mylite_prepare(
    mylite_db *db,
    const char *sql,
    size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail);

int mylite_step(mylite_stmt *stmt);
int mylite_reset(mylite_stmt *stmt);
int mylite_finalize(mylite_stmt *stmt);
```

Return values:

- `MYLITE_ROW`: a row is available.
- `MYLITE_DONE`: execution is complete.
- Any other result code: execution failed.

```c
#define MYLITE_NUL_TERMINATED ((size_t)-1)
```

`sql_len == MYLITE_NUL_TERMINATED` means `sql` is NUL-terminated. `tail`, when
non-NULL, receives the first uncompiled byte.

Initial implementation status: ordinary prepared statements run through
MariaDB's embedded `MYSQL_STMT` API; MyLite-owned prepared savepoint-control
statements use the direct transaction checkpoint path, and prepared transaction
lifecycle controls mirror the supported bounded direct transaction lifecycle
after MariaDB prepared execution succeeds. Prepared single-marker
transaction-related `SET` assignments mirror supported `autocommit`,
`completion_type`, transaction read-only, and transaction-isolation setup after
their bound values are validated and MariaDB prepared execution succeeds. The
implementation supports one
statement per prepare call, 1-based scalar parameter binding, row stepping,
reset/finalize ownership, parameter counts, affected rows, insert ids, MariaDB
diagnostics, warnings after completed execution and selected failed execution
paths, and binary-safe text/BLOB column reads. File-backed MyLite
storage-engine builds synchronize
successful prepared `CREATE/DROP DATABASE` and `CREATE/DROP SCHEMA` statements
plus prepared `ALTER DATABASE` / `ALTER SCHEMA` option changes with the schema
namespace catalog. Prepared execution also covers the supported foreign-key
subset for prepared FK table creation, successful FK-constrained DML, and
child/parent row-check diagnostics, including bounded scalar-parameter FK DML.
Rich parameter metadata is not exposed on the current MariaDB base because
`mysql_stmt_param_metadata()` is reserved and returns no metadata. Multi-result
execution, array binding, streaming parameter binding, parser-derived parameter
metadata, and direct-execution large-value streaming remain planned.

## Bindings

Bind indexes are 1-based.

```c
typedef void (*mylite_destructor)(void *);

#define MYLITE_STATIC    ((mylite_destructor)0)
#define MYLITE_TRANSIENT ((mylite_destructor)(-1))

unsigned mylite_bind_parameter_count(mylite_stmt *stmt);
int mylite_clear_bindings(mylite_stmt *stmt);

int mylite_bind_null(mylite_stmt *stmt, unsigned index);
int mylite_bind_int64(mylite_stmt *stmt, unsigned index, long long value);
int mylite_bind_uint64(mylite_stmt *stmt, unsigned index, unsigned long long value);
int mylite_bind_double(mylite_stmt *stmt, unsigned index, double value);
int mylite_bind_text(
    mylite_stmt *stmt,
    unsigned index,
    const char *value,
    size_t value_len,
    mylite_destructor destructor);
int mylite_bind_blob(
    mylite_stmt *stmt,
    unsigned index,
    const void *value,
    size_t value_len,
    mylite_destructor destructor);
```

`MYLITE_STATIC` borrows bytes until the statement is reset, rebound, or
finalized. `MYLITE_TRANSIENT` copies bytes before the call returns. A custom
destructor is called after MyLite no longer needs the input.

The current implementation clears bindings on `mylite_reset()`, so callers
should rebind before the next execution. This keeps borrowed `MYLITE_STATIC`
input lifetimes explicit.

Typed date, time, decimal, JSON, and geometry bindings can be added without
forcing those values through text.

## Columns

```c
typedef enum mylite_value_type {
  MYLITE_TYPE_NULL = 0,
  MYLITE_TYPE_INT64 = 1,
  MYLITE_TYPE_UINT64 = 2,
  MYLITE_TYPE_DOUBLE = 3,
  MYLITE_TYPE_TEXT = 4,
  MYLITE_TYPE_BLOB = 5
} mylite_value_type;

unsigned mylite_column_count(mylite_stmt *stmt);
const char *mylite_column_name(mylite_stmt *stmt, unsigned column);
const char *mylite_column_database_name(mylite_stmt *stmt, unsigned column);
const char *mylite_column_table_name(mylite_stmt *stmt, unsigned column);
const char *mylite_column_origin_table_name(mylite_stmt *stmt, unsigned column);
const char *mylite_column_origin_name(mylite_stmt *stmt, unsigned column);
unsigned mylite_column_mariadb_type(mylite_stmt *stmt, unsigned column);
unsigned mylite_column_flags(mylite_stmt *stmt, unsigned column);
unsigned mylite_column_charset(mylite_stmt *stmt, unsigned column);
unsigned mylite_column_decimals(mylite_stmt *stmt, unsigned column);
unsigned long mylite_column_length(mylite_stmt *stmt, unsigned column);
unsigned long mylite_column_max_length(mylite_stmt *stmt, unsigned column);
mylite_value_type mylite_column_type(mylite_stmt *stmt, unsigned column);

long long mylite_column_int64(mylite_stmt *stmt, unsigned column);
unsigned long long mylite_column_uint64(mylite_stmt *stmt, unsigned column);
double mylite_column_double(mylite_stmt *stmt, unsigned column);
const char *mylite_column_text(mylite_stmt *stmt, unsigned column);
const void *mylite_column_blob(mylite_stmt *stmt, unsigned column);
size_t mylite_column_bytes(mylite_stmt *stmt, unsigned column);
int mylite_column_read(
    mylite_stmt *stmt,
    unsigned column,
    size_t offset,
    void *buffer,
    size_t buffer_len,
    size_t *out_read);
```

Column indexes are 0-based. Column values are valid until the next
`mylite_step()`, `mylite_reset()`, or `mylite_finalize()` on that statement.

The current implementation maps MariaDB integer result fields to signed or
unsigned 64-bit values, float/double fields to double, BLOB-family fields to
BLOB, and string/date/time/decimal fields to TEXT. TEXT values are
NUL-terminated for `mylite_column_text()`, but `mylite_column_bytes()` remains
the authoritative byte count.

The metadata accessors expose the MariaDB result metadata copied at prepare
time. Alias names come from `mylite_column_name()`. Database, table, original
table, and original column names may be empty for expressions and literal
columns. `mylite_column_mariadb_type()` returns MariaDB's native
`enum_field_types` numeric value; flags, charset, decimals, display length, and
maximum observed length follow MariaDB's `MYSQL_FIELD` metadata.

`mylite_column_read()` copies a byte range from the current TEXT/BLOB value
into caller-owned memory. It returns `MYLITE_OK` with `*out_read == 0` for SQL
`NULL` values or offsets at or beyond the column length. The full pointer APIs
still materialize statement-owned values when callers need a stable pointer for
the current row.

## Diagnostics And Warnings

```c
typedef enum mylite_warning_level {
  MYLITE_WARNING_NOTE = 1,
  MYLITE_WARNING_WARNING = 2,
  MYLITE_WARNING_ERROR = 3
} mylite_warning_level;

int mylite_errcode(mylite_db *db);
int mylite_extended_errcode(mylite_db *db);
unsigned mylite_mariadb_errno(mylite_db *db);
const char *mylite_sqlstate(mylite_db *db);
const char *mylite_errmsg(mylite_db *db);
unsigned mylite_warning_count(mylite_db *db);
int mylite_warning(
    mylite_db *db,
    unsigned index,
    mylite_warning_level *level,
    unsigned *code,
    const char **message);
```

`mylite_errcode()` is the stable MyLite classification. MariaDB errno and
SQLSTATE remain available for callers that need server-compatible diagnostics.

`mylite_warning()` uses a zero-based index. It returns `MYLITE_NOTFOUND` when
the requested warning is not stored.

Initial implementation status: successful direct execution stores the
structured rows returned by `SHOW WARNINGS`. Prepared non-result statements
expose warnings after `mylite_step()` returns `MYLITE_DONE`; prepared result
statements expose warnings after the result set is drained. Failed direct
execution, failed prepare, and failed prepared execute paths also retain
structured warning rows before a prepared result set is active. Fetch-time
failure warning capture remains planned.

## Statement Effects

```c
long long mylite_changes(mylite_db *db);
unsigned long long mylite_last_insert_id(mylite_db *db);
```

These are part of observable MariaDB application behavior. Exposing them through
MyLite avoids forcing callers into raw MariaDB handles.

Initial implementation status: direct and prepared execution update the last
insert id after successful statements and report affected rows for successful
non-result statements. Direct execution covers temporary-table
insert/update/delete effects, prepared execution covers parameterized
insert/update/delete effects, and result-producing statements report zero
changed rows.

## Memory Ownership

```c
void mylite_free(void *ptr);
```

Heap memory returned by MyLite is released with `mylite_free()`. Text returned
from handle or statement accessors remains owned by that handle or statement
unless the API explicitly says otherwise.

## Configuration

```c
int mylite_busy_timeout(mylite_db *db, unsigned milliseconds);
int mylite_set_durability(mylite_db *db, int durability);
int mylite_limit(mylite_db *db, int limit_id, long long value);
```

`mylite_open_config.busy_timeout_ms` configures primary-file advisory lock
waits during open. `mylite_busy_timeout()` updates the same handle-local
setting for later SQL work. A zero timeout keeps immediate `MYLITE_BUSY`
behavior; a nonzero timeout retries cooperating primary-file lock conflicts
until the lock is acquired or the timeout expires.

Durability modes:

```c
#define MYLITE_DURABILITY_FULL   2
#define MYLITE_DURABILITY_NORMAL 1
#define MYLITE_DURABILITY_OFF    0
```

The public API exposes MyLite concepts, not raw `my.cnf` option names.

## Threading

- A `mylite_db` handle is not used concurrently unless configured for
  serialized mode.
- Different handles may be used on different threads.
- Handles opened on the same file coordinate through the shared file runtime.
- Cooperating storage opens use primary-file advisory locks to reject unsafe
  cross-process readers, writers, and recovery with busy errors after the
  configured busy timeout expires; pending rollback and transaction journals are
  recovered while holding that lock.
- Full cross-process multi-writer behavior and SQL table, row, named-lock, and
  transaction lock integration remain planned.

SQLite-style threading modes can be added when backed by tests.

## Server Features Outside The Core API

The core file-owned API rejects or omits server-owned features that do not fit
the embedded library model:

- network users and authentication,
- replication, binlog, replication/binlog filter assignments, and disabled
  binlog/replication system-variable assignments,
- Galera/wsrep,
- dynamic plugin installation and loading,
- dynamic UDF registration, loading, lookup, and execution,
- external durable storage engines,
- `CREATE TEMPORARY TABLE` foreign-key DDL, `SET DEFAULT` FK actions, and
  broader foreign-key actions outside the supported MyLite subset,
- partition DDL until partition metadata, partition routing, and per-partition
  lifecycle semantics exist,
- filesystem-backed views, triggers, and routines until they have MyLite
  catalog storage,
- sequence objects and sequence value surfaces such as `NEXT VALUE FOR`,
  `PREVIOUS VALUE FOR`, `NEXTVAL()`, `LASTVAL()`, and `SETVAL()` until they
  have MyLite catalog storage and durable value persistence,
- SQL locking statements and named-lock functions until MyLite has real table,
  row, named-lock, and transaction-aware lock semantics,
- online and in-place ALTER forms until MyLite has online DDL and lock
  integration,
- server audit plugins,
- `LOAD DATA` / `LOAD XML` file import, including network-protocol
  `LOAD DATA LOCAL`,
- `LOAD_FILE()` and `SELECT ... INTO OUTFILE` / `DUMPFILE` host-file SQL I/O,
- table-maintenance and key-cache administration SQL such as `CHECK TABLE`,
  `ANALYZE TABLE`, `OPTIMIZE TABLE`, `REPAIR TABLE`, their representative
  `LOCAL` / `NO_WRITE_TO_BINLOG` forms, `CACHE INDEX`, and
  `LOAD INDEX INTO CACHE`,
- external backup SQL such as `BACKUP STAGE`, `BACKUP LOCK`, and
  `BACKUP UNLOCK`,
- query cache administration such as `FLUSH QUERY CACHE`,
  `RESET QUERY CACHE`, and query-cache system-variable assignment,
- zlib-backed SQL, protocol, binlog, and compressed-column storage,
- native InnoDB storage-engine runtime and its tablespaces, redo, undo, and
  dictionary state,
- native CSV storage-engine runtime and `.CSV` / `.CSM` table files,
- unsupported explicit `ENGINE=...` table creation and engine-change requests,
- native partition storage-engine wrapper runtime,
- statement profiling such as `SHOW PROFILE`, `SHOW PROFILES`,
  profiling system-variable assignment, and `INFORMATION_SCHEMA.PROFILING`,
- optimizer trace diagnostics such as optimizer-trace system-variable
  assignment and `INFORMATION_SCHEMA.OPTIMIZER_TRACE`,
- static SHOW information such as `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
  `SHOW PRIVILEGES`,
- status metadata such as `SHOW STATUS`, `SHOW GLOBAL STATUS`,
  `SHOW SESSION STATUS`, `INFORMATION_SCHEMA.GLOBAL_STATUS`, and
  `INFORMATION_SCHEMA.SESSION_STATUS`; these surfaces are queryable but return
  no rows in the default embedded profile,
- process-list metadata such as `SHOW PROCESSLIST` and
  `SHOW FULL PROCESSLIST`; `INFORMATION_SCHEMA.PROCESSLIST` is queryable but
  returns no rows in the default embedded profile,
- SQL `HANDLER` commands,
- SQL `HELP`,
- `SELECT ... PROCEDURE`, including `PROCEDURE ANALYSE()`,
- view runtime and metadata surfaces; `INFORMATION_SCHEMA.VIEWS` is queryable
  but returns no rows until MyLite has catalog-backed view metadata,
- stored-program compiler/runtime surfaces behind routines, packages, events,
  compound statements, and `CALL`; routine status and routine
  information-schema metadata are queryable but return no rows until MyLite has
  catalog-backed routine metadata,
- trigger runtime and metadata surfaces; `SHOW TRIGGERS` and
  `INFORMATION_SCHEMA.TRIGGERS` are queryable but return no rows until MyLite
  has catalog-backed trigger metadata,
- server utility functions such as `BENCHMARK()`, `SLEEP()`, `UUID_SHORT()`,
  `MASTER_POS_WAIT()`, and `MASTER_GTID_WAIT()`,
- Oracle SQL mode,
- XML SQL functions `EXTRACTVALUE()` and `UPDATEXML()`,
- GIS SQL functions such as `ST_AsText()`, `ST_GeomFromText()`,
  `ST_Contains()`, `PointFromText()`, `Point()`, and `X()`,
- vector SQL functions such as `VEC_FromText()`, `VEC_ToText()`,
  `VEC_DISTANCE()`, `VEC_DISTANCE_EUCLIDEAN()`, and
  `VEC_DISTANCE_COSINE()`,
- MariaDB-specific `SFORMAT()` SQL function,
- `JSON_SCHEMA_VALID()` SQL function,
- `JSON_TABLE()` table function,
- MariaDB dynamic-column SQL functions,
- sequence value surfaces such as `NEXT VALUE FOR`, `PREVIOUS VALUE FOR`,
  `NEXTVAL()`, `LASTVAL()`, and `SETVAL()`,
- event scheduler,
- performance schema.

Representative account, event, plugin, replication, binlog,
replication/binlog filter assignment, binlog/replication system-variable
assignment, view, trigger, routine, package,
sequence, `CALL`, UDF `CREATE FUNCTION ... SONAME`,
global autocommit-control, parameterized or otherwise unsupported
transaction-control `SET` forms, unsupported transaction-control, release
completion defaults, XA,
SQL locking,
named-lock, SQL `HELP`, SQL `HANDLER`, `SELECT ... PROCEDURE`, SQL file-I/O,
table-maintenance/key-cache administration, statement profiling, external
backup SQL, query cache administration, optimizer trace, static SHOW
information, process-list metadata, zlib compression, server utility function,
Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function,
SFORMAT SQL function, JSON schema validation function, JSON table function,
dynamic column function, SQL sequence value surface, partition,
`CREATE TEMPORARY TABLE` foreign-key DDL, broader unsupported foreign-key
action commands including `SET DEFAULT`, plus unsupported explicit
`ENGINE=...` table creation and engine-change requests, are rejected before
MariaDB execution with stable MyLite errors. The default
embedded profile also links fail-closed stubs for stored-program runtime
symbols that retained MariaDB parser or cleanup paths still reference, omits
dynamic UDF lookup/execution bodies, omits unsupported binlog event-root,
log-event server runtime, replication GTID-state runtime, MyISAM maintenance
source objects, native InnoDB engine objects, native CSV engine objects, and
native partition wrapper objects, omits native CSV, InnoDB, MyISAM,
MRG_MyISAM, and partition engine
registration and disabled binlog/replication plus native-MyISAM-only
system-variable registration, omits the JSON schema-validation source object,
replaces replication filter runtime, JSON table-function execution,
dynamic-column packed BLOB runtime, SQL handler command execution, SQL sequence
runtime, external backup command execution, query cache runtime, zlib-backed
compression, vector SQL and MHNSW runtime, and dynamic plugin loading with
fail-closed or disabled stubs/configuration, and compiles embedded binlog
transaction and event-write entry points to no-ops. Other
unsupported surfaces should fail with
stable MyLite result codes and MariaDB diagnostics where possible.

## Compatibility Adapter

A later adapter can expose the MariaDB C API:

```c
MYSQL *mylite_mysql_handle(mylite_db *db);
```

That adapter is for existing code. It does not define the primary lifetime,
configuration, file ownership, or storage semantics.
