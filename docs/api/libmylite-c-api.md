# libmylite C API

`libmylite` is the primary embedded API. It follows SQLite's handle ownership
style while preserving MariaDB SQL behavior, diagnostics, warnings, affected
rows, insert ids, and richer data types.

## Principles

- Open a database by directory path.
- Prefer database directory names ending in `.mylite`, such as `app.mylite/`.
  This is a convention, not an enforced suffix.
- Own all handles explicitly.
- Do not require a daemon, socket, server account, password handshake, or
  network connection for local embedded use.
- Keep MariaDB internals private.
- Expose stable MyLite result codes and MariaDB-compatible diagnostics.
- Make prepared statements reusable and binary safe.
- Keep raw `MYSQL *` access out of the core lifetime model.

## Handles

```c
typedef struct mylite_db mylite_db;
typedef struct mylite_stmt mylite_stmt;
```

`mylite_db` owns one logical embedded MariaDB connection to one MyLite database
directory. Multiple handles for the same directory coordinate through a shared
directory runtime. The current embedded implementation supports one open
database directory per process at a time; opening a different directory while
the runtime is active returns `MYLITE_BUSY`. A second process opening the same
durable directory for read/write access also returns `MYLITE_BUSY` while the
directory lock is held.

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

int mylite_open(
    const char *path,
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

`mylite_open()` takes explicit open flags and an optional configuration pointer.
Pass `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE` with `NULL` configuration for
the default read/write-create behavior.
`mylite_open_config.size` makes the struct growable without breaking ABI.
`MYLITE_OPEN_READONLY` currently returns `MYLITE_MISUSE`; it is reserved until
the native-storage lifecycle can enforce read-only access through MariaDB engine
configuration.

Once statement handles exist, `mylite_close()` returns `MYLITE_BUSY` when
statements or dependent resources still exist. Deferred close can be added
separately if a real use case appears.

Initial implementation status: open/close is backed by MariaDB embedded startup
when the `embedded-dev` CMake preset enables it. MyLite passes owned startup
options, ignores ambient option files with `--no-defaults`, establishes the
requested MyLite database directory, and creates the baseline layout:
`mylite.meta`, `mylite.lock`, `datadir/`, `tmp/`, and `run/`.

Existing directories must either already be valid MyLite directories or be empty
and opened with `MYLITE_OPEN_CREATE`. A pre-existing empty directory without
`MYLITE_OPEN_CREATE` returns `MYLITE_NOTFOUND`. A non-empty directory without
valid `mylite.meta`, or with missing required layout directories, returns
`MYLITE_CORRUPT`.

For durable database paths, the embedded runtime starts with MariaDB native
storage under the database directory: `--datadir=<db>/datadir`,
`--tmpdir=<db>/tmp`, `--plugin-dir=<db>/run/plugins`, and
`--aria-log-dir-path=<db>/datadir`. InnoDB data, redo, undo, and temporary
paths are also pinned under `datadir/` and `tmp/`. Server topology and account
surfaces are disabled with startup options such as `--skip-grant-tables`,
`--skip-networking`, `--skip-log-bin`, and `--skip-slave-start`; Performance
Schema is omitted by the default build profile or disabled when a custom build
includes it. The final close removes `run/` and clears temporary files under
`tmp/`; durable metadata and table files remain in `datadir/`. `mylite.lock` is
an advisory lock anchor and may remain after close or process exit. A clean
open replaces stale inactive `run/` state after taking the directory lock.
`mylite_open_config.temp_directory` is currently used only by the `:memory:`
bootstrap path.

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
SQL `NULL` values as `NULL` callback entries, and populates MariaDB diagnostics
on query failure. Native-storage smoke coverage verifies controlled
`ENGINE=MyISAM` DDL and DML persist across close and reopen. Explicit
`ENGINE=InnoDB` coverage verifies commit, rollback, savepoints, clean reopen,
and child-process recovery through SQL transaction statements. Additional engine
coverage verifies explicit InnoDB, MyISAM, Aria, MEMORY, and default-engine
table creation plus representative WordPress-shaped InnoDB DDL. Broader DDL
forms remain later slices.

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

Initial implementation status: `mylite_prepare()` wraps MariaDB prepared
statements in embedded builds. `tail` is set to the end of the resolved SQL
text on successful single-statement prepares. `mylite_close()` returns
`MYLITE_BUSY` while statements are active.

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

Initial implementation status: MyLite copies text and blob bytes during binding
for all destructor modes. Custom destructors are called after the copy. Text may
use `MYLITE_NUL_TERMINATED`; blob bindings require an explicit byte length.
Passing `NULL` with an explicit zero byte length binds an empty text or blob
value, not SQL `NULL`; use `mylite_bind_null()` for SQL `NULL`. Bindings may be
changed before the first `mylite_step()` or after a successful `mylite_reset()`.

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
mylite_value_type mylite_column_type(mylite_stmt *stmt, unsigned column);

long long mylite_column_int64(mylite_stmt *stmt, unsigned column);
unsigned long long mylite_column_uint64(mylite_stmt *stmt, unsigned column);
double mylite_column_double(mylite_stmt *stmt, unsigned column);
const char *mylite_column_text(mylite_stmt *stmt, unsigned column);
const void *mylite_column_blob(mylite_stmt *stmt, unsigned column);
size_t mylite_column_bytes(mylite_stmt *stmt, unsigned column);
```

Column indexes are 0-based. Column values are valid until the next
`mylite_step()`, `mylite_reset()`, or `mylite_finalize()` on that statement.
The first implementation exposes primary value classes and byte counts; richer
metadata remains future work.

MariaDB exposes richer type metadata than this primary value classification.
Later metadata APIs should expose original MariaDB field type, charset,
collation, signedness, precision, scale, schema, table, and original column
name where applications need them.

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
The default embedded profile uses a compact MariaDB server error-message
catalog: common diagnostics such as syntax errors and duplicate-key errors stay
readable, while less common inherited server errors may return generic message
text. Callers that need exact server compatibility should rely on MariaDB errno
and SQLSTATE rather than matching the full upstream message catalog.

`mylite_warning()` uses a zero-based index. It returns `MYLITE_NOTFOUND` when
the requested warning is not stored.

Initial implementation status: warning count comes from MariaDB and indexed
warning lookup reads `SHOW WARNINGS` on demand, returning level, code, and
message text owned by the database handle.

## Statement Effects

```c
long long mylite_changes(mylite_db *db);
unsigned long long mylite_last_insert_id(mylite_db *db);
```

These are part of observable MariaDB application behavior. Exposing them through
MyLite avoids forcing callers into raw MariaDB handles.

Initial implementation status: direct execution updates the last insert id after
successful statements and reports affected rows for successful non-result
statements. Result-producing statements report zero changed rows.

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
- Handles opened on the same directory coordinate through the shared directory
  runtime.
- Cross-process read/write opens are rejected with `MYLITE_BUSY` while another
  process owns the directory lock.
- Multiple-reader and concurrent-writer modes remain planned until read-only
  startup and engine-specific concurrency behavior are covered.

SQLite-style threading modes can be added when backed by tests.

## Features Outside The Core API

The core directory-owned API rejects or omits server-owned and optional
compatibility features that do not fit the embedded library model:

- network users and authentication,
- replication, relay log, and binary-log runtime,
- Galera/wsrep,
- dynamic plugin installation and shared-object loading,
- dynamic UDF shared-library registration,
- durable storage outside the MyLite database directory,
- server audit plugins,
- network-protocol `LOAD DATA LOCAL`,
- PROXY protocol listener support,
- event scheduler,
- performance schema,
- Oracle SQL mode,
- Oracle compatibility function aliases,
- SQL help-table lookup,
- statement profiling,
- query-cache management,
- optimizer trace diagnostics,
- general and slow query logs,
- statement digest diagnostics,
- server status variables,
- process-list metadata,
- user statistics diagnostics,
- the optional `SFORMAT()` SQL helper,
- legacy `PROCEDURE ANALYSE()` SELECT diagnostics,
- static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES`
  information.

Top-level SQL command families for users, roles, grants, password changes,
dynamic plugins, events, replication, binlog administration, and foreign-server
metadata are rejected before direct execution or prepared-statement
preparation. The default embedded profile starts with binary logging disabled
and compiles binlog transaction, row-event, and GTID-state entry points to
embedded no-ops where they are unreachable through supported MyLite behavior.
It also omits the unsupported injector root that is only needed by the server
topology runtime, plus guarded replication execution system variables such as
`slave_type_conversions` and `rpl_semi_sync_master_enabled`. Replication and
binlog filter variables such as `replicate_do_db`,
`replicate_wild_ignore_table`, and `binlog_do_db` are also omitted because the
core embedded profile has no replication or binary-log topology to filter.
Dynamic UDF registration through `CREATE FUNCTION ... SONAME` is rejected
through the same policy because it loads server-owned shared libraries and
persists metadata in server system tables. Attempts to enable Oracle SQL mode,
SQL `HELP`, statement-profiling commands, and query-cache management commands
are also rejected through the same policy and fail with `MYLITE_ERROR` and a
stable MyLite diagnostic. Oracle compatibility function aliases such as
`DECODE_ORACLE` and `oracle_schema` routing are omitted from the default
embedded profile; ordinary MySQL/MariaDB string functions remain available.
Omitted function aliases fail as unsupported SQL function paths. Query-cache
SELECT hints remain accepted no-op syntax. `INFORMATION_SCHEMA.PROFILING` is
rejected as part of the statement-profiling server diagnostic surface.
Optimizer trace variables and
`INFORMATION_SCHEMA.OPTIMIZER_TRACE`, including unqualified reads while
`information_schema` is the current schema, are rejected as server diagnostics.
Ordinary planning, execution, and `EXPLAIN` remain supported. The default
embedded profile omits general and slow query logs as daemon diagnostics;
query-log variables and log flush commands are rejected while SQL errors,
warnings, and result metadata remain available. Statement digest normalization
is omitted because it feeds Performance Schema diagnostics; startup sets
`@@max_digest_length=0`, and ordinary statement execution, prepared
statements, diagnostics, and `EXPLAIN` remain available. The default embedded
profile also omits server status-variable publication, so `SHOW STATUS` and
status Information Schema tables return empty result sets while ordinary SQL
diagnostics, warnings, result metadata, and the public C API remain available.
Process-list metadata is also omitted: `SHOW PROCESSLIST` and
`SHOW FULL PROCESSLIST` are rejected as daemon/session inventory, while
`INFORMATION_SCHEMA.PROCESSLIST` stays visible and returns zero rows.
User statistics diagnostics are omitted as optional server counters:
`userstat`, the userstat Information Schema tables, and `FLUSH *_STATISTICS`
are rejected or absent, while ordinary application tables with the same names
remain usable outside `information_schema`.
User-variable diagnostics are omitted as optional session introspection:
`INFORMATION_SCHEMA.USER_VARIABLES`, `SHOW USER_VARIABLES`, and
`FLUSH USER_VARIABLES` are rejected, while ordinary `@variable` SQL remains
available.
Foreign-server metadata is omitted as server-global remote connection
configuration: `CREATE SERVER`, `ALTER SERVER`, `DROP SERVER`, and
`SHOW CREATE SERVER` are rejected, and the default embedded archive omits the
`mysql.servers` metadata cache.
External backup runtime SQL is omitted as a server backup-tool surface:
`BACKUP STAGE`, `BACKUP LOCK`, and `BACKUP UNLOCK` are rejected, while
ordinary DDL hooks stay inert.
VIO TLS transport is omitted from the default embedded archive because
`libmylite` does not open a socket or perform a network handshake. Wire-protocol
adapters that need TLS should own that transport explicitly. Inherited
`mysql_ssl_set()` calls fail closed in this profile, while retained SQL crypto
functions continue to use OpenSSL Crypto.
The default embedded profile also omits `SFORMAT()`, which fails as an
unknown SQL function; ordinary `FORMAT()` remains available. The legacy
`PROCEDURE ANALYSE()` SELECT extension is rejected as an unsupported diagnostic
surface while ordinary SELECT queries remain supported. Long system-variable
help comments are omitted from the default embedded profile; `SHOW VARIABLES`
and system-variable values remain available, but
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty. Startup
variables also cover disabled binlog, performance schema, query cache,
query logging, statement profiling, grant tables, networking,
`@@have_dynamic_loading=NO`, and the transient database-local plugin directory.
Guarded replication execution variables are omitted from `SHOW VARIABLES` and
`@@` lookup in the default embedded profile, while `@@log_bin=0` remains
available as compatibility evidence. Replication and binlog filter variables
are omitted from the same surfaces.
PROXY protocol listener support is omitted for the same serverless-core reason:
the default embedded profile has no socket listener, and
`proxy_protocol_networks` is absent from `SHOW VARIABLES` and `@@` lookup.
The default embedded archive omits runtime shared-object plugin loading while
keeping static built-in plugins and native storage engines available. Static
`SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` are rejected as
server-information surfaces; ordinary supported `SHOW` commands remain
available.

## Compatibility Adapter

A later adapter can expose the MariaDB C API:

```c
MYSQL *mylite_mysql_handle(mylite_db *db);
```

That adapter is for existing code. It does not define the primary lifetime,
configuration, file ownership, or storage semantics.
