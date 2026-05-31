# Ownerless Cross-Process Concurrency

## Problem Statement

MyLite currently protects a durable database directory with one cross-process
exclusive `mylite.lock`. That is safe, but it is not enough for applications
that use many independent OS processes, such as PHP-FPM workers, and expect
them all to open the same database and run read/write SQL concurrently.

This spec researches whether MyLite can provide full read and write concurrency
without a coordinating owner process, daemon, socket broker, or hidden server.
The requested shape is:

- every process links `libmylite`,
- every process opens the same `<name>.mylite/` directory directly,
- coordination happens only through files, byte-range locks, memory-mapped
  shared files, and durable recovery state inside the MyLite database
  directory,
- no process has a special permanent owner role,
- MariaDB SQL behavior and native storage formats remain the compatibility
  target.

## Conclusion

SQLite's WAL model is a useful pattern, but it is not a drop-in answer for
MariaDB/InnoDB.

SQLite was designed around a pager that owns page cache, locking, journaling,
and recovery. In WAL mode, readers and one writer coordinate through a WAL file
and a memory-mapped wal-index file in the same directory. SQLite WAL still has
important limits: it requires same-host shared memory, can return busy during
recovery or cleanup, supports many readers with one WAL writer, and checkpoints
can be starved by long readers.

MyLite should use the same broad primitive shape: a file-backed
`mmap(MAP_SHARED)` region inside the `.mylite` directory, plus byte-range locks
and durable logs. That is real shared memory between processes after mapping,
but it keeps identity, permissions, copy/delete behavior, and stale-state
cleanup attached to the database directory. External shared-memory objects can
be considered later as an optimization, but they should not be the default
lifecycle model.

MariaDB/InnoDB is different. InnoDB's concurrency is native and strong inside
one server process, but it is implemented with process-local global state:
metadata-lock maps, transaction lists, active read-write transaction hashes,
record-lock queues, buffer-pool dirty pages and latches, redo-log append state,
checkpoint state, purge state, dictionary cache, and recovery state. InnoDB also
contains file-lock paths that explicitly warn when another process is using the
same InnoDB files. Those paths are not enough for MyLite ownerless concurrency,
but they are important startup and safety constraints that must be replaced only
after directory-owned coordination is active.

Therefore ownerless cross-process writers are possible only if MyLite turns the
relevant MariaDB/InnoDB process-global coordination into directory-owned
coordination. That is not a small locking change. It is a new multi-process
InnoDB runtime mode, with a persistent/shared transaction, lock, page-version,
redo, checkpoint, dictionary, and recovery protocol.

The plan below is the least compromised way to implement the requested shape.
It preserves the "no coordinating process" rule, but it does not preserve the
current small fork delta. It is a large, high-risk storage-engine project.

## Non-Goals

- Do not add a hidden owner process, proxy process, daemon, broker, socket
  server, or background process that outlives the opening application process.
- Do not rely on one process permanently owning the directory.
- Do not claim network filesystem support. SQLite WAL itself requires
  same-host shared memory for normal WAL mode; MyLite should require local
  filesystem semantics until proven otherwise.
- Do not remove InnoDB, JSON, GEOMETRY, ordinary DDL/DML, transactions, or
  other important compatibility features.
- Do not implement a custom SQL engine.
- Do not claim this can be a bounded short-term slice.

## Source Findings

### SQLite WAL And SHM

- SQLite WAL keeps original content in the main database file and appends
  changes into a WAL file. Commit is represented by a commit record in the WAL.
  Source: <https://www.sqlite.org/wal.html>.
- Readers remember an "end mark" and see a stable snapshot by reading the last
  relevant page version at or before that mark. Source:
  <https://www.sqlite.org/wal.html#concurrency>.
- The wal-index accelerates page lookup and is implemented as a memory-mapped
  file in the same directory as the database. SQLite documents that same-host
  shared memory is required for normal WAL mode. Source:
  <https://www.sqlite.org/wal.html#implementation_of_shared_memory_for_the_wal_index>.
- SQLite WAL supports concurrent readers and a writer, but there is only one
  WAL file and only one writer at a time. Source:
  <https://www.sqlite.org/wal.html#concurrency>.
- Checkpointing can run concurrently with readers but must stop at reader end
  marks; long-running readers can make the WAL grow. Source:
  <https://www.sqlite.org/wal.html#avoiding_excessively_large_wal_files>.
- The WAL-index uses explicit lock bytes for write, checkpoint, recovery, and
  reader slots. Source: <https://www.sqlite.org/walformat.html#wal_locks>.
- SQLite rollback-mode locking also shows that SQLite's concurrency is a pager
  design: SHARED, RESERVED, PENDING, and EXCLUSIVE locks gate access to the
  database file. Source: <https://www.sqlite.org/lockingv3.html>.

### MariaDB And MySQL Documentation

- MariaDB's `mariadbd-multi` documentation warns against multiple server
  processes sharing the same data directory and recommends separate data
  directories unless the operator knows exactly what they are doing. Source:
  <https://mariadb.com/docs/server/server-management/starting-and-stopping-mariadb/mariadbd-multi>.
- MySQL's multiple data directory documentation says two servers should not
  update the same databases, and even with precautions this only applies to
  MyISAM and MERGE, not other storage engines. Source:
  <https://dev.mysql.com/doc/refman/8.0/en/multiple-data-directories.html>.
- MariaDB documents InnoDB as an ACID transactional storage engine with
  row-level locking, crash recovery, transaction logging, undo/MVCC, purge, and
  online DDL. Source:
  <https://mariadb.com/docs/server/server-usage/storage-engines/innodb/innodb-storage-engine-introduction>.
- MariaDB documents InnoDB row locks, gap/next-key style behavior, and
  intention locks as storage-engine concurrency behavior. Source:
  <https://mariadb.com/docs/server/server-usage/storage-engines/innodb/innodb-lock-modes>.

### MariaDB 11.8.6 Source

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). MyLite first-party source
findings refer to the current branch paths.

- `mariadb/storage/innobase/os/os0file.cc:340` implements
  `os_file_lock()` with `fcntl(F_SETLK)`. Its error message tells users to
  check for another `mariadbd` process using the same InnoDB data or log files.
- `mariadb/storage/innobase/os/os0file.cc:1006` calls `os_file_lock()` for
  read/write InnoDB file opens when that path's locking conditions are enabled.
- `mariadb/storage/innobase/fsp/fsp0sysspace.cc:475` and
  `mariadb/storage/innobase/fil/fil0fil.cc:367` also lock system tablespace
  files under their own `my_disable_locking`, read-only, and space-id
  conditions. Phase 0 must record actual behavior under MyLite's startup vector
  instead of assuming all InnoDB file opens behave the same way.
- `mariadb/sql/sys_vars.cc:3329` defines `skip_external_locking` over
  `my_disable_locking`, and `mariadb/sql/mysqld.cc:8987` derives
  `my_disable_locking` from `opt_external_locking`. Ownerless mode must
  introduce an explicit MyLite/InnoDB file-lock policy instead of depending on
  inherited `skip_external_locking` semantics.
- `mariadb/storage/innobase/srv/srv0start.cc:1304` starts InnoDB. Startup
  initializes process-global state with `fil_system.create()`,
  `buf_pool.create()`, `log_sys.create()`, `recv_sys.create()`, and
  `lock_sys.create()` at lines 1434-1442.
- `mariadb/storage/innobase/srv/srv0start.cc:1350` states that InnoDB embedded
  startup is not cleanly restartable inside one process without MyLite's local
  restart patches. That is separate from cross-process safety, but it shows
  the engine was not designed as a lightweight per-handle component.
- `mariadb/sql/mdl.cc:171` defines `MDL_map`, the singleton collection of all
  metadata locks in a server. `mariadb/sql/mdl.cc:702` stores it as a static
  process-global `mdl_locks`.
- `mariadb/sql/mdl.h:878` defines `MDL_context` per server connection, with
  metadata lock tickets in process memory.
- `mariadb/storage/innobase/include/lock0types.h:167` defines record/table
  `ib_lock_t` objects protected by `lock_sys.latch`, with `trx_t *` pointers
  to process-local transaction objects.
- `mariadb/storage/innobase/lock/lock0lock.cc:357` defines process-global
  `lock_sys`.
- `mariadb/storage/innobase/include/trx0sys.h:853` defines the transaction
  system central memory structure `trx_sys_t`; `trx_sys` is process-global at
  line 1329.
- `mariadb/storage/innobase/include/trx0sys.h:859` stores the next transaction
  ID in a process-local atomic counter. `register_rw()` at line 1166 inserts
  transactions into a process-local `rw_trx_hash`, and `snapshot_ids()` at line
  1087 builds MVCC read views from that in-memory hash.
- `mariadb/storage/innobase/include/log0log.h:132` defines process-local
  `log_t`, including log LSN allocation, log buffer, write LSN, checkpoint
  LSNs, writer function pointer, and redo file handle.
- `mariadb/storage/innobase/include/buf0buf.h:1275` and later define buffer
  pool page-fix, hash, LRU, and dirty-page state guarded by process-local
  latches and mutexes.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:356` commits mini-transaction log
  records, writes page `FIL_PAGE_LSN`, and inserts dirty pages into the flush
  list. Page-version design must follow mini-transaction/page-LSN boundaries,
  not just SQL transaction boundaries.
- `mariadb/storage/innobase/include/read0types.h:35` describes read views as
  transaction-ID visibility sets, and
  `mariadb/storage/innobase/row/row0sel.cc:953` checks clustered-record
  visibility against a read view. Nearby row-selection paths use undo records
  to construct visible versions. Cross-process page visibility must preserve
  that MVCC/undo behavior.

### mmap, Locks, And Wait Primitives

- `mmap(MAP_SHARED)` maps file bytes into a process address space such that
  updates are visible to other processes mapping the same region. For precise
  persistence to the underlying file, `msync()` or a stronger filesystem sync
  path is required. Source:
  <https://www.man7.org/linux/man-pages/man2/mmap.2.html>.
- Linux futexes are 32-bit words placed in shared memory. The normal fast path
  is user-space atomic access, with the kernel used only to block and wake
  waiters under contention. Source:
  <https://www.man7.org/linux/man-pages/man2/futex.2.html>.
- POSIX process-shared mutexes allow a mutex in shared memory to be operated on
  by threads from different processes. Source:
  <https://www.man7.org/linux/man-pages/man3/pthread_mutexattr_getpshared.3p.html>.
- Robust mutexes can report owner death with `EOWNERDEAD`, but support and
  exact behavior must be validated per platform before relying on them in the
  MyLite shared-memory backend. Source:
  <https://www.man7.org/linux/man-pages/man3/pthread_mutexattr_setrobust.3.html>.
- POSIX `fcntl` byte-range locks are advisory and automatically released when
  a process terminates. Classic process-associated locks also have a dangerous
  close behavior: closing any descriptor for the file can release all of that
  process's locks on the file. Linux open-file-description locks have better
  close semantics but are Linux-specific and do not currently provide kernel
  deadlock detection. Source:
  <https://man7.org/linux/man-pages/man2/fcntl_locking.2.html>.
- MariaDB already carries mmap helpers. `mariadb/include/my_sys.h:1053`
  maps `my_mmap()` to `mmap()` / `mmap64()` when available, and
  `mariadb/mysys/my_mmap.c:21` notes that MariaDB's `my_msync()` follows
  `msync()` with file sync because `msync()` alone only syncs mapped pages to
  the filesystem cache.
- MariaDB's inherited mmap-backed `TC_LOG_MMAP` transaction coordinator at
  `mariadb/sql/log.cc:10859` is useful evidence for mmap lifecycle and commit
  grouping, but it is not reusable as ownerless MyLite coordination. It uses
  process-local `PAGE` objects, process-local mutexes and condition variables,
  and was designed for external two-phase commit recovery inside one server
  process. The default MyLite profile currently omits that runtime because
  external XA is outside the embedded core API.

## Required Semantics

The final ownerless mode must provide these observable guarantees:

- Multiple processes can open the same durable MyLite directory read/write.
- Readers in any process see committed data according to MariaDB/InnoDB
  isolation rules.
- Non-conflicting writers in different processes can proceed concurrently.
- Conflicting writers block, timeout, or deadlock according to InnoDB rules.
- DDL and DML coordinate through MariaDB-compatible metadata locks across
  processes.
- Commit, rollback, savepoint, crash recovery, purge, foreign keys, generated
  columns, online DDL, and temporary objects behave consistently across
  processes.
- Any process can crash at any point; a later opener recovers without an owner
  process.
- All durable and transient coordination files live inside `<name>.mylite/`.
- The mode can be disabled or rejected on filesystems without required local
  mmap and locking semantics.

## Directory Layout

The current layout is:

```text
app.mylite/
  mylite.meta
  mylite.lock
  datadir/
  tmp/
  run/
```

Ownerless concurrency needs additional directory-owned coordination state:

```text
app.mylite/
  mylite.meta
  mylite.lock
  datadir/
  tmp/
  run/
  concurrency/
    mylite-concurrency.meta
    mylite-concurrency.lock
    mylite-concurrency.shm
    mylite-concurrency.wal
    mylite-concurrency.ckpt
    process/
      <process-slot>.heartbeat
```

Roles:

- `mylite-concurrency.meta`: durable concurrency format version, MariaDB base
  ref, page size, checksum/version policy, feature flags, and clean/dirty
  state.
- `mylite-concurrency.lock`: byte-range lock anchor, not an exclusive database
  lock. Individual bytes/ranges protect recovery, shared-memory rebuild, log
  append, checkpoint, dictionary changes, tablespace allocation, process slot
  allocation, and read slots.
- `mylite-concurrency.shm`: memory-mapped shared state. It is transient and can
  be rebuilt from durable logs, but while active it is the common coordination
  memory for all processes.
- `mylite-concurrency.wal`: MyLite multi-process coordination log. This is not
  a replacement for InnoDB redo. Its fixed recovery header is followed by the
  ownerless page-version payload. Guarded ownerless SQL now appends dirty page
  images before the temporary commit-LSN flush bridge releases shared locks.
  The shared page-version index can rebuild and checkpoint those records.
  Guarded ownerless SQL can use page-version reads for direct or prepared
  `SELECT`/`WITH` statements at a live page-version read LSN, while the
  page-visible LSN remains the durable recovery/checkpoint boundary. Repeatable
  read and serializable transactions pin that live read LSN on their first
  consistent read. Transactions that already performed local writes or locking
  reads avoid global refresh, and clean-page refresh skips locally dirty buffer
  pages.
  DML/DDL, recovery, checkpointing, and tablespace replay still use the
  conservative native-file bridge until broader recovery is implemented.
- `mylite-concurrency.ckpt`: durable checkpoint/progress metadata for rebuilding
  shared coordination state.
- `process/*.heartbeat`: process-liveness evidence for crash detection. These
  are hints only; correctness must come from OS locks and durable recovery.

## mmap Coordination Design

### Decision

Use `concurrency/mylite-concurrency.shm` as a file-backed shared-memory object
opened from the database directory and mapped with `MAP_SHARED`.

This is different from external `shm_open()` or SysV shared memory:

- the database directory contains the object name and permissions,
- copying or deleting a closed database directory handles the shared-memory
  file naturally,
- stale coordination state is visible to later openers,
- two path spellings of the same directory still converge on the same inode,
- the file can be locked and validated with the same directory identity rules
  as the rest of MyLite.

The `.shm` file is not durable truth. It is a rebuildable live index and wait
surface. Correctness after crash must come from durable metadata, InnoDB files,
the MyLite coordination log, and checkpoint records. A later opener may discard
and rebuild `.shm` after acquiring the recovery lock.

Copying or backing up a database directory while any process has it open remains
unsupported until a backup protocol exists. A copied closed directory may carry
a stale `.shm` file; the opener must treat it as disposable, validate it
against `mylite.meta` and `mylite-concurrency.meta`, and rebuild it if any live
process, generation, clean-shutdown, or format evidence is inconsistent.

### Stable Shared-Memory ABI

The shared-memory format must be an explicit MyLite ABI, not a dump of C++
objects or MariaDB structs.

Rules:

- Store only fixed-width integer fields, byte arrays, and offsets from the
  mapping base.
- Never store raw pointers, vtable pointers, STL containers, MariaDB object
  addresses, `pthread_mutex_t`, `pthread_cond_t`, or other opaque system
  objects in the stable format.
- Treat all pointers in per-process code as caches over stable IDs and offsets.
- Align hot structures to cache-line boundaries and keep independent counters
  on separate cache lines.
- Include a byte order marker and reject incompatible architecture formats
  until cross-endian sharing is explicitly designed.
- Include header fields for format version, minimum/maximum supported version,
  page size, cache-line size chosen by MyLite, database UUID, MariaDB base ref
  hash, feature flags, mapping size, segment table offset, segment table count,
  shared-memory generation, recovery generation, checkpoint generation, and
  clean/dirty/rebuilding state.
- Use monotonic generation counters for every read-mostly structure so readers
  can detect concurrent rebuild, resize, or invalidation.
- Use checksums on durable logs and checkpoints. The `.shm` header may have a
  diagnostic checksum, but recovery must not rely on it as durable evidence.
- Use explicit acquire/release memory ordering for shared latch words,
  generation counters, and published offsets. The stable ABI should expose
  aligned `uint32_t` and `uint64_t` words and operate on them with compiler or
  platform atomic intrinsics; it should not persist C++ `std::atomic<T>` object
  layout. Platforms where required atomic widths are not lock-free must use the
  slower byte-range-lock backend or reject ownerless mode.

The first mapping should be one page-aligned region with a fixed-size header
and a segment table. Segments can then grow independently while remaining
addressed by offsets:

```text
mylite_shm_header
mylite_shm_segment[]
process slots
wait channels
transaction table
metadata-lock table
InnoDB lock table
dictionary generation table
page-version index
redo/coordination-log append index
checkpoint/read-view slots
statistics and diagnostics
```

The current foundation implements the fixed header and the first coordination
segments. It uses magic `MYLSHM01`, format/min-format version fields, header
size, byte-order marker, feature flags, clean/dirty/rebuilding state, mapping
size, shared-memory and recovery generation counters, segment-table
offset/count, and the database UUID copied from `mylite-concurrency.meta`. The
active segments are a fixed process registry with 16 fixed-size slots, a fixed
wait-channel table with 16 fixed-size channels, a fixed MDL lock-table segment,
a fixed transaction-registry segment with 64 transaction slots, a fixed
read-view registry, a fixed InnoDB table/record lock registry, a
redo-visibility state segment, a page-version index segment, a
dictionary-generation segment, and a separate page-write lock registry for
internal X/SX page-latch write ownership, plus an ownerless AUTO_INCREMENT
high-watermark registry keyed by InnoDB table ID. Current opens publish one
active process slot for the embedded runtime process, assign that slot the
wait-channel range, mark `.shm` dirty while the runtime is active, and release
the slot before returning `.shm` to clean state on final close. Clean opens
preserve the existing segments. A later open treats dirty, rebuilding, invalid,
or no-live-process stale state as volatile coordination state, rebuilds the
segments, and increments the recovery-generation field. Hot registry latches
use a fixed-width 32-byte MyLite latch ABI with an atomic packed state/owner-slot
word, wake epoch, waiter count, owner generation, and owner-death diagnostics.
Stable registry APIs pass the process-slot generation into MDL, transaction,
read-view, InnoDB lock, and redo-visibility coordination so stale owner-slot
reuse is not treated as valid ownership. Cleanup of a dead owner with active
MDL, transaction, read-view, InnoDB lock, or redo-visibility state is
deliberately blocked while
another live process remains, because those entries are recovery evidence. A
new opener receives busy instead of deleting them; after no live owners remain,
the next open rebuilds volatile shared state and lets the MariaDB/InnoDB
runtime recover normally. Guarded ownerless SQL opens take a narrow
`SYSTEM_TABLES` byte-range lock around embedded runtime bootstrap and the core
`mysql.*` compatibility-table bootstrap, preventing two processes from racing
InnoDB startup table locks or Aria-backed `CREATE TABLE IF NOT EXISTS`
statements during open. Volatile process-registry active/live counters are read
through `MAP_SHARED` mappings, not ordinary file reads, so recovery decisions do
not rebuild live peer state from stale file-cache observations. Page-version
segments are active in the production `.shm` layout for rebuild and checkpoint
bookkeeping, and `.shm` rebuilds replay durable page-version WAL records back
into that index. Guarded ownerless SQL allows page-version reads for direct or
prepared `SELECT`/`WITH` statements at a live page-version read LSN while the
page-visible LSN remains the durable recovery/checkpoint boundary. Repeatable
read and serializable transactions pin the live read LSN on their first
consistent read, while transactions with local writes or locking reads avoid
global refresh and clean-page refresh skips locally dirty buffer pages. The
transaction registry has latch-protected
monotonic transaction ID allocation, active transaction snapshots sorted for
future read-view construction, oldest-active tracking, stale end rejection, and
owner-scoped active-count checks over file-backed `MAP_SHARED` mappings.
Durable opens map the `.shm` file with `MAP_SHARED` to validate the published
layout before starting MariaDB. InnoDB now has guarded MyLite hook surfaces for
transaction ID allocation, read-write transaction registration, transaction
serialisation-number assignment, active-ID snapshots, maximum transaction ID
reads, deregistration, read-view publication, table/record locks, waits,
AUTO_INCREMENT high-watermark reads/publishes, and redo visibility. Internal
or recovered InnoDB transactions that were never
registered in the ownerless shared transaction registry still receive
serialisation numbers from the shared monotonic sequence, and missing
deregistration is treated as a no-op; registered read-write transactions
continue through the shared registry.
The dictionary-generation segment serializes ownerless DDL with an odd/even
generation counter. Peers wait for active DDL to finish before starting a new
statement; when the generation changes, they flush SQL table caches and evict
unused InnoDB dictionary-cache entries before reopening tables against the
latest shared page visibility.

### Mapping Lifecycle

Open sequence:

1. Resolve and validate the MyLite database directory identity.
2. Open `mylite-concurrency.lock` with `O_CLOEXEC`.
3. Acquire the `OPEN_REGISTRY` byte-range lock.
4. Open or create `mylite-concurrency.meta`.
5. Open or create `mylite-concurrency.shm` with `O_RDWR | O_CREAT | O_CLOEXEC`.
6. If the file is shorter than the required initial size, acquire
   `SHM_RESIZE`, extend with `ftruncate()`, and fsync the containing directory
   after creation.
7. Map with `MAP_SHARED`; use `MAP_SHARED_VALIDATE` on Linux when requesting
   Linux-specific flags.
8. Validate header, database UUID, format, mapping size, feature flags, and
   generation.
9. If the header is absent, incompatible, dirty after a crash, or marked
   rebuilding, acquire `RECOVERY`, rebuild shared memory from durable state,
   publish a new generation, and release waiters.
10. Allocate a process slot, publish the process generation, and mark the slot
    active only after the runtime can either complete open or recover cleanly.

Directory identity must include a durable database UUID in `mylite.meta`, the
device/inode pair observed by the opener, and a concurrency-generation field in
`mylite-concurrency.meta`. The UUID survives closed-directory copies; process
slots do not. If device/inode evidence changes but the UUID matches, the opener
must assume the directory may have been copied or moved and must rebuild
volatile `.shm` state before use.

Close sequence:

1. Mark the process slot closing.
2. Release all local wait registrations.
3. Deregister active transactions only after their durable commit/rollback
   state is settled.
4. Publish a closing generation and clear the process slot.
5. Unmap the region and close descriptors.

Resize sequence:

- Never shrink `.shm` while any process may still have the file mapped.
- Grow only while holding `SHM_RESIZE`.
- Publish the new mapping size and generation after `ftruncate()` succeeds.
- Processes that observe a larger generation remap at a safe point.
- If resize fails, keep the old mapping valid and return a clear capacity or
  no-memory error rather than corrupting shared structures.

Durable metadata and checkpoint writes must use an atomic write protocol:
write a new file or generation record, fsync it, atomically rename or publish
the generation, and fsync the parent directory. `.shm` updates do not need this
durable protocol unless they are also represented in `.meta`, `.wal`, or
`.ckpt`.

### Synchronization Model

Use two classes of synchronization.

Durable/recovery gates use byte-range locks in `mylite-concurrency.lock`.
These locks are slower, but the kernel releases them when a process exits, and
they do not leave opaque shared-memory mutex bytes stuck forever. They protect:

- recovery,
- shared-memory initialization and resize,
- format upgrades,
- process-slot allocation,
- durable checkpoint publication,
- durable log truncation,
- dictionary/table-space allocation boundaries.

Hot-path coordination uses shared-memory latch words and generation counters.
The stable MyLite latch format should be small fixed-width fields:

- atomic packed state/owner process-slot word,
- owner generation,
- wait epoch,
- waiter count,
- optional recursion/debug fields compiled out of release builds.

Linux should use futex wait/wake on these words for the high-performance
backend. Platforms without a proven futex-like primitive should start with
byte-range locks plus bounded adaptive backoff, then add a platform backend
only after stress tests prove wakeup and owner-death behavior.

Opaque `pthread_mutex_t` / `pthread_cond_t` objects should not be part of the
stable `.shm` ABI. They can be considered for a backend-specific volatile
segment only if process-shared and robust behavior is verified on that
platform. The portable design should work without them.

Lock ordering must be explicit and test-enforced:

```text
RECOVERY
  SHM_RESIZE
    OPEN_REGISTRY
      PERSISTED_CONFIG
        DICT
          MDL
            TRX_SYS
              LOG_APPEND reservation
                LOCK_SYS bucket
                  PAGE_VERSION bucket
```

No thread may block on SQL execution, fsync, or user callbacks while holding a
global shared-memory latch. Long waits must enqueue a stable wait record,
release the latch, then sleep on the platform wait primitive or retry loop.
`LOG_APPEND` in the ordering above means range reservation and in-memory
publication only; fsync and slow disk I/O must happen after the hot latch is
released. Checkpoint work must not sit under ordinary hot-path latches. It must
claim a checkpoint generation, visit buckets incrementally with try-lock or
short bounded locks, and back off rather than blocking foreground writers for a
full scan.

### Process And Wait Registry

Each process slot must contain:

- slot generation,
- process ID,
- optional boot ID,
- executable/runtime generation,
- open mode,
- current state,
- heartbeat timestamp,
- last observed shared-memory generation,
- first active transaction ID,
- oldest read-view end mark,
- cleanup cursor,
- per-process wait-channel range.

PIDs are never sufficient alone because they can be reused. Any liveness check
must compare process slot generation and boot/start evidence. Heartbeats are
diagnostic and cleanup hints; correctness comes from byte-range locks, durable
logs, and generation-checked recovery.

Wait records must be stable across processes:

- waiting transaction ID,
- waiting process slot/generation,
- wait class,
- waited object stable ID,
- wake epoch,
- timeout deadline,
- victim/deadlock result.

Wakeups must be best-effort optimizations. A missed wakeup must degrade to a
bounded timeout/rescan, not a permanent hang.

### Durable State Boundary

The `.shm` file may cache:

- active process slots,
- active transaction table,
- metadata-lock queues,
- InnoDB lock queues,
- page-version index,
- checkpoint read slots,
- dictionary generations,
- wait queues,
- statistics.

The `.shm` file must not be the only copy of:

- committed transaction state,
- redo or page-version records needed for recovery,
- checkpoint positions,
- dictionary changes,
- tablespace allocation changes,
- compatibility or format metadata.

Every publication from private process state into `.shm` must have an ordered
durable story:

1. reserve stable IDs or log space,
2. write enough durable intent or recovery metadata,
3. publish to shared memory,
4. make the SQL-visible state available,
5. checkpoint or retire old state only after all reader slots allow it.

### Performance Model

The design must be fast in the common case:

- Read-mostly paths should use generation counters or seqlocks so uncontended
  readers avoid kernel calls.
- Hash tables for MDL, transactions, record locks, and page versions should be
  sharded by bucket with independent latch words.
- Process slots, hot counters, and bucket latches should be cache-line padded
  to avoid false sharing.
- Latches should spin briefly only when the owner is running on CPU, then park
  with futex or the platform wait backend.
- Log append should reserve ranges with atomics where possible and batch fsyncs
  through group commit.
- Page-version lookup should be O(1) average by `(space_id, page_no)` with a
  short version chain filtered by reader end mark.
- Checkpoint should advance incrementally and never scan the whole mapping
  while holding a global latch.
- Long-running readers should be visible in read slots so checkpoint pressure
  can be diagnosed and eventually surfaced as a busy or checkpoint-starvation
  condition.
- Metrics should track latch contention, futex sleeps, lock waits, checkpoint
  stalls, page-version chain length, and recovery/rebuild time.

This performance model still preserves the no-owner-process rule. Any process
can perform recovery, checkpoint, or group-commit work when it wins the
corresponding transient lock.

### Platform Policy

Initial platform support should be explicit:

- Linux local filesystems are the primary high-performance target because
  `MAP_SHARED`, futexes, and byte-range locks can cover the intended design.
- macOS/APFS can support the directory-backed `mmap` and byte-range-lock
  correctness model, but the high-performance wait backend must be validated
  separately. Until then, macOS ownerless mode should be experimental or use
  the slower lock/backoff backend.
- Windows needs a separate backend using `CreateFileMapping`, byte-range locks,
  and a Windows wait primitive such as `WaitOnAddress` before support can be
  claimed.
- Network filesystems remain unsupported unless a later slice proves mmap
  coherence, byte-range locks, fsync semantics, and stale-client behavior.

The open path must run a capability probe before enabling ownerless mode:

- create and map a test region in the database directory,
- verify cross-process `MAP_SHARED` visibility,
- verify byte-range lock conflict and release-on-death behavior,
- verify wait/wake backend if enabled,
- verify file growth/remap behavior,
- reject the mode with a precise diagnostic if any required primitive fails.

## Architecture Options

### Option A: Keep Current Exclusive Directory Lock

This is safe and already implemented, but it does not satisfy the requested
concurrency goal.

### Option B: Shared Read-Only Opens Only

Multiple read-only processes can share the directory while one writer is
excluded. This is a useful intermediate slice, but it does not satisfy the
requested write-concurrency goal.

### Option C: SQLite-Like Outer Page WAL Around InnoDB

Add a page-version WAL beneath InnoDB so all processes read page images from
the WAL before table files, similar to SQLite. This is conceptually closest to
SQLite but highly invasive:

- InnoDB normal reads do not consult redo for committed data; they read pages
  from the buffer pool or tablespace files.
- InnoDB can hold committed dirty pages only in one process's buffer pool. A
  second process cannot see those pages unless they are flushed to tablespace
  files or exposed through a shared page-version log.
- A page-version WAL would have to hook every InnoDB page read/write, checksum,
  doublewrite, flush, checkpoint, purge, and recovery path.
- This would create a second physical logging layer beside InnoDB redo and undo.

This could preserve native table files as the eventual checkpointed format, but
it would be a major fork. It would also still need global transaction, lock,
MDL, dictionary, and recovery coordination.

### Option D: Multi-Process InnoDB Coordination Layer

Move or mirror the relevant process-global InnoDB and SQL-layer state into
directory-backed shared memory and durable logs. Per-process runtimes still run
MariaDB, but all storage-engine concurrency decisions use shared state.

This is the most direct plan for preserving InnoDB semantics. It is the plan of
record for the requested shape.

### Option E: Shared Buffer Pool

Put the InnoDB buffer pool itself in shared memory, including page descriptors,
page hash, latches, dirty lists, flush lists, and LRU state. This avoids a
second page WAL, but it is very invasive and introduces pointer/allocator,
robust-mutex, ABI, crash-cleanup, and ASLR problems. It may become necessary
for performance later, but it should not be the first design.

## Plan Of Record

Implement Option D first, with a possible Option C page-version layer if source
experiments prove per-process buffer pools cannot be made coherent without it.

The core rule is:

> No process owns the database. Every process owns its connection, and the
> directory owns coordination state.

### Process Registry

Add a process registry in `mylite-concurrency.shm`:

- fixed or extendable process slots,
- process ID, boot ID where available, executable generation, start timestamp,
  heartbeat timestamp, and open mode,
- robust recovery state for "opening", "active", "closing", "crashed",
- per-process wait-channel ranges,
- per-process transaction lists and cleanup cursors.

Correctness must not depend only on heartbeats. Heartbeats help diagnostics and
orphan cleanup, while byte-range locks and durable logs decide ownership of
critical sections.

### Shared Lock Primitives

Define a portable MyLite lock and wait abstraction:

- POSIX baseline: byte-range `fcntl` locks for recovery and durable exclusion.
  Prefer Linux open-file-description locks when available because classic
  process-associated locks can be released by closing an unrelated descriptor
  for the same file.
- Hot path: MyLite-owned fixed-width latch words in `mmap` shared memory, with
  explicit memory ordering and no opaque system object in the stable ABI.
- Linux: futex wait/wake on the latch words.
- macOS: validate the byte-range-lock/backoff backend first; add a better wait
  primitive only after platform stress coverage exists.
- Windows: file mapping, byte-range lock, and wait backend in a later port.
- Process-shared robust `pthread_mutex_t` / `pthread_cond_t` can be evaluated
  as a backend-specific volatile optimization, but not as the portable shared
  format.

Required lock classes:

- `RECOVERY`: one process rebuilds shared memory and runs crash recovery.
- `SHM_RESIZE`: one process grows or remaps shared-memory segments.
- `OPEN_REGISTRY`: process slot allocation and cleanup.
- `LOG_APPEND`: global redo/coordination log append reservation.
- `CHECKPOINT`: checkpoint and shared-memory backfill progress.
- `DICT`: dictionary cache generation and DDL changes.
- `MDL`: metadata lock map mutations and wait queues.
- `LOCK_SYS`: InnoDB record/table lock hash mutations.
- `TRX_SYS`: transaction ID allocation and active transaction registration.
- `SPACE_ALLOC`: tablespace, segment, and page allocation metadata.
- `PERSISTED_CONFIG`: metadata format and engine configuration changes.

These locks replace the current directory-wide exclusive lock only after each
critical path is covered by tests.

### InnoDB File-Lock Policy

The current exclusive `mylite.lock` protects the whole directory before MariaDB
starts. Ownerless mode cannot simply remove that lock and set
`skip_external_locking`; that would bypass inherited file-lock checks without
replacing the transaction, lock, page-visibility, and recovery state that made
single-process InnoDB safe.

Ownerless mode needs an explicit file-lock policy:

- keep existing MariaDB/InnoDB file locking unchanged in exclusive mode,
- keep current `mylite.lock` while opening modes are exclusive or shared
  read-only,
- add a MyLite ownerless startup path that disables or bypasses only the
  inherited InnoDB file locks that conflict with multiple processes after
  `RECOVERY` and shared-memory validation have succeeded,
- fail ownerless open if any file-lock path outside the designed policy is
  still active,
- test the exact MyLite startup vector because MariaDB gates file locking with
  different `my_disable_locking`, read-only, and space-id conditions in
  different paths.

This policy must be implemented before product-enabling ownerless read/write
opens. Until then, all multi-process writer work stays behind test-only gates.

### Cross-Process Transaction System

Replace process-local transaction visibility with directory-owned visibility:

- `trx_sys.m_max_trx_id` becomes a shared atomic or lock-protected counter in
  `mylite-concurrency.shm`, checkpointed durably.
- `rw_trx_hash` becomes a shared active transaction table keyed by transaction
  ID, process slot, thread/connection ID, and state.
- MVCC read-view creation must snapshot active transactions across all
  processes, not just the current process.
- Transaction deregistration must not release visibility state until commit or
  rollback is durably represented.
- Crash recovery must identify active transactions from crashed processes and
  roll them back or recover prepared state according to InnoDB rules.

Source impact:

- `trx_sys_t::get_new_trx_id_no_refresh()`
- `trx_sys_t::register_rw()`
- `trx_sys_t::deregister_rw()`
- `trx_sys_t::snapshot_ids()`
- read view creation in `read/read0read.cc`
- purge visibility and oldest-view logic

### Cross-Process Record And Table Lock Manager

Move InnoDB lock queues into shared state or mirror them there:

- table locks,
- record locks,
- gap locks,
- insert-intention locks,
- predicate/page locks used by spatial indexes,
- wait queues,
- deadlock detector graph,
- timeout and victim selection.

The shared lock table must not store raw process pointers. Every object ID must
be stable across processes:

- stable lock transaction ID,
- process slot,
- table ID,
- index ID,
- page ID,
- heap number or record identifier,
- lock mode bits.

Local `trx_t *`, `dict_index_t *`, and `ib_lock_t *` pointers can only be
per-process caches around stable shared IDs.

Source impact:

- `lock_sys_t`
- `ib_lock_t`
- `lock_rec_*` and `lock_table_*` paths
- deadlock detection in `lock0lock.cc`
- transaction wait and wakeup condition handling

### Cross-Process Metadata Locks

MariaDB's MDL subsystem is a process-global singleton today. Ownerless
concurrency requires a shared MDL map:

- object keys must be stable and serialized into shared memory,
- granted/waiting queues must be process-neutral,
- repeated local tickets must be reference counted so one release cannot drop
  another still-live logical holder from the same process,
- local upgrades must allow one process to hold weaker and stronger tickets for
  the same key while still blocking incompatible holders from other processes,
- waiters must be wakeable across processes,
- deadlock detection must traverse cross-process waits,
- DDL must invalidate dictionary/table caches in every process.

Source impact:

- `MDL_map`
- `MDL_lock`
- `MDL_context`
- DDL acquisition paths in `sql_table.cc`, `sql_alter.cc`, procedure/view/trigger
  metadata paths, and stored-routine metadata.

### Page Visibility And Buffer Coherency

This is the hardest part.

Inside one process, InnoDB can commit while dirty pages remain in the buffer
pool because later readers in that same server see the same buffer pool and
because crash recovery uses redo. Across processes, a different process has a
different buffer pool. It cannot see another process's committed dirty page
unless one of the following is true:

1. The writer flushes all commit-relevant pages to tablespace before making the
   transaction visible.
2. The reader can find committed page versions in a shared page-version log.
3. All processes share the same buffer pool.

Option 1 is not acceptable as the final design. It would be very slow, would
change InnoDB's writeback assumptions, and would still need careful protection
against readers observing uncommitted flushed pages.

Option 2 is the likely required design:

- every persistent page image that needs to be visible outside a process is
  versioned by `(space_id, page_no, page_lsn)` in a directory-owned
  page-version log or equivalent structure,
- SQL transaction commit publishes transaction visibility and commit ordering;
  it is not the only page-version boundary because InnoDB page changes happen
  through mini-transactions,
- readers consult the page-version index before reading a tablespace page and
  still apply InnoDB MVCC/undo visibility rules using the shared transaction
  state,
- undo pages, insert/delete-mark state, purge state, and dictionary pages must
  be visible with the same rules as clustered/index pages,
- checkpoint copies safe page versions back into native tablespace files only
  after all reader slots and purge/checkpoint rules allow it,
- long readers can delay checkpoint progress,
- recovery rebuilds the page-version index from durable logs.

This is SQLite-like, but adapted below InnoDB's page layer rather than above a
simple SQLite pager. It must coexist with InnoDB redo/undo and read-view
semantics unless a later design proves a safe replacement for selected redo
responsibilities in MyLite's embedded profile.

Option 3 may be a later performance optimization but is unsafe as the first
approach because process-shared buffer-pool pointers, latches, memory
allocation, and crash cleanup would deeply rewrite InnoDB internals.

### Cross-Process Redo, Commit, And Checkpoint

At minimum:

- LSN allocation must be global.
- Redo append reservations must be serialized or atomically reserved across
  processes.
- Group commit must work across processes or degrade safely to serialized fsync.
- Checkpoint state must be global and durable.
- Log resize and log format changes must be single-writer operations.
- Recovery must run once and block other opens until complete.

If a page-version log is added, define its relation to InnoDB redo:

- Phase 1: keep InnoDB redo as the crash-recovery source for native files;
  page-version log is for cross-process visibility.
- Phase 2: prove crash recovery can rebuild page-version index and reconcile
  with InnoDB redo after any process crash.
- Phase 3: decide whether some redo responsibilities can be simplified in the
  embedded profile. Do not do this until exhaustive crash tests pass.

The phase labels above are local to the redo/page-version design, not product
support stages. Product ownerless read/write support cannot be advertised until
page visibility and crash recovery are proven together.

### Dictionary And Tablespace Coordination

DDL and space allocation need directory-wide coordination:

- table ID allocation,
- space ID allocation,
- file creation, rename, discard/import, truncate, and drop,
- persistent dictionary changes,
- foreign-key metadata,
- generated-column metadata,
- online DDL state,
- table-definition cache invalidation,
- handler open/close generation checks.

The current SQL-layer MDL map only protects one process. It must coordinate
with InnoDB dictionary latches and shared DDL generations before cross-process
DDL can be supported.

### MyISAM, Aria, MEMORY, And Other Engines

The first ownerless mode should be InnoDB-only for durable user tables.

Reasons:

- MyISAM and Aria have different file and lock behavior and lack InnoDB MVCC.
- MySQL documentation says shared-data-directory updates are only even
  theoretically limited to MyISAM/MERGE, not InnoDB, but MyLite's modern
  default engine is InnoDB and application workloads expect InnoDB semantics.
- SQL-layer MDL, DDL, and system metadata still need cross-process coordination
  regardless of engine.

Policy:

- Existing MyISAM/Aria support remains available in exclusive mode.
- Ownerless mode rejects durable MyISAM/Aria writes until dedicated engine
  designs exist.
- MEMORY remains per-runtime and should be rejected or namespaced in
  ownerless durable mode unless explicitly designed.

## Coverage Policy

Extensive coverage is part of the feature, not follow-up work.

Rules:

- A phase is not complete unless implementation, documentation, compatibility
  status, and tests land together.
- Ownerless read/write must not be exposed through a public product path until
  deterministic, crash/fault, application, stress, and platform gates pass.
- Each phase must include fast deterministic tests in CI.
- Any phase that changes crash, concurrency, recovery, or filesystem behavior
  must also add a multi-process, fault-injection, or stress variant.
- MariaDB comparison tests are required when observable SQL, lock wait,
  deadlock, isolation, DDL, recovery, or diagnostic behavior changes.
- Long-running randomized and stress tests may run under separate labels or a
  nightly profile, but their commands, seeds, environment, and triage policy
  must be documented before support is claimed.
- Coverage must include positive and negative behavior, including unsupported
  filesystem, platform, engine, and feature-gate cases.
- Fault-injection points must be named and stable enough for failed cases to be
  replayed.

## Implementation Plan

### Phase 0: Negative Proof And Measurement

Purpose: prove current upstream assumptions and capture baselines.

Tasks:

1. Add an experimental test-only build switch that disables MyLite's
   directory-wide lock without changing InnoDB.
2. Start two embedded runtimes over one test directory and confirm InnoDB file
   locking rejects, allows, or fails under the actual MyLite startup vector.
3. Record exact inherited file-lock conditions involving `my_disable_locking`,
   `srv_read_only_mode`, space IDs, and the relevant InnoDB file paths.
4. Record exact failure modes for InnoDB, MyISAM, Aria, and default-engine
   tables.
5. Add no production behavior.

Exit criteria:

- A test document shows why the current engine cannot simply share a directory.
- No release build exposes unsafe multi-process writes.

Initial verification command:

```sh
ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
```

The `ownerless-test-hooks` preset is intentionally separate from normal
developer and embedded builds. It enables only a test-only directory-lock
bypass used to prove inherited MariaDB/InnoDB/Aria startup behavior without
turning that path into product behavior.

### Phase 1: Concurrency Format And Capability Gating

Tasks:

1. Add `mylite-concurrency.meta` and an explicit directory format extension.
2. Add a durable database UUID and concurrency-generation field to MyLite
   directory metadata.
3. Add C API capability reporting:
   - `MYLITE_CAP_SAME_PROCESS_CONCURRENCY`,
   - `MYLITE_CAP_SHARED_READONLY`,
   - `MYLITE_CAP_OWNERLESS_RW`.
4. Add open flags or config:
   - exclusive mode,
   - shared read-only mode,
   - ownerless read/write mode.
5. Reject ownerless mode unless the filesystem and platform support required
   byte-range locks and mmap semantics.

Exit criteria:

- No behavior change by default.
- Unsupported platforms fail clearly.

### Phase 2: Same-Process Multi-Handle Concurrency

Tasks:

1. Add multi-threaded tests with multiple `mylite_db` handles in one process.
2. Cover non-conflicting InnoDB writers, conflicting row locks, deadlocks,
   lock wait timeout, rollback, savepoints, DDL waiting on DML, and FK checks.
3. Run comparison cases against MariaDB server where practical.

Exit criteria:

- MyLite proves inherited MariaDB/InnoDB concurrency inside one embedded
  runtime before attempting cross-process coordination.

### Phase 3: Shared Read-Only Opens

Tasks:

1. Add shared read-only opens through the ownerless runtime.
   `MYLITE_OPEN_READONLY | MYLITE_OPEN_SHARED_READONLY` now skips the
   process-wide `mylite.lock`, uses the same per-process ownerless `run/` and
   `tmp/` layout as ownerless writers, starts MariaDB with server
   `@@read_only=ON`, publishes process/read-view state in the directory-owned
   coordination files, and can observe commits from ownerless read/write peers.
   The embedded runtime records one access mode per process, so a same-process
   ownerless read/write open is rejected while a shared read-only runtime is
   live.
2. Enforce user-visible read-only SQL on the handle.
   Current coverage rejects DDL, DML, prepared DML, write-transaction
   requests, and locking reads with `MYLITE_READONLY` before execution.
   Shared read-only ownerless coverage also verifies prepared reads and a
   repeatable-read read-only snapshot while a peer ownerless writer commits.
   A true InnoDB `innodb_read_only` startup mode remains a separate hardening
   task: ownerless page-version refresh currently advances local redo
   visibility when active writer peers publish newer pages, which is not
   compatible with InnoDB's native read-only shutdown invariant.
3. Fail read-only open if recovery is required and no writer/recovery opener is
   available.
   Shared read-only opens now return `MYLITE_BUSY` when stale ownerless
   shared-memory state contains recovery-sensitive transactions, locks, redo
   progress, or DDL ownership; a read/write ownerless opener must perform that
   recovery first.
4. Allow multiple read-only processes with or without an active ownerless
   writer.

Exit criteria:

- Shared read-only handles can query an existing database and observe committed
  ownerless writer changes.
- Read-only handles reject user-visible writes. They may write MyLite
  coordination state under `concurrency/`.

### Phase 4: Ownerless Recovery And Shared-Memory Foundation

Tasks:

1. Add `concurrency/` directory.
2. Add `mylite-concurrency.meta`, `mylite-concurrency.lock`, and
   `mylite-concurrency.shm` creation and validation.
3. Add the stable shared-memory header, segment table, format validation,
   database UUID binding, generation counters, and dirty/rebuilding states.
   The current code has the stable header, format validation, UUID binding,
   generation fields, segment-table population, an exclusive-mode process
   registry, wait channels, and an MDL lock-table foundation segment.
   Dirty/rebuilding rebuild transitions are implemented for volatile `.shm`
   state, and durable opens validate the layout through `MAP_SHARED`;
   shared-memory preparation now takes `RECOVERY` before `SHM_RESIZE`; `.wal`
   and `.ckpt` files exist with UUID-bound headers. Durable coordination records
   include page-version payloads after the `.wal` header and redo/page-visible
   LSNs in `.ckpt`. `MYLITE_CAP_OWNERLESS_RW` is exposed in embedded builds now
   that SQL lock, transaction, page-visibility, and guarded recovery paths use
   ownerless coordination.
4. Add byte-range lock protocol for `RECOVERY`, `SHM_RESIZE`,
   `OPEN_REGISTRY`, `PERSISTED_CONFIG`, durable checkpoint publication, and
   durable log truncation.
5. Add a Linux futex-backed latch/wait backend for shared-memory hot paths,
   with a portable byte-range-lock/backoff backend for platforms without a
   proven wait primitive. The current code has a private mapped latch wait
   backend with Linux futex wakeups where available and adaptive timeout
   backoff elsewhere. Hot registry latches now use a fixed-width MyLite latch
   word that records owner slot/generation and wake metadata; primitive tests
   cover cross-process wakeup and dead-owner reporting, and SQL-facing
   ownerless MDL, transaction, read-view, InnoDB lock, and redo-visibility
   paths pass the runtime process-slot generation into those latches. Full
   owner-death recovery still depends on durable recovery records and page
   visibility.
6. Add process slots with slot generations, process identity, open mode,
   heartbeat, oldest read-view marker, cleanup cursor, and wait-channel range.
   The current code writes those fields for the single exclusive runtime
   process and has an internal cross-process allocator/releaser with generation
   checks, heartbeat updates, callback-driven stale-slot cleanup, and cleanup
   evidence for a process that exits without releasing its slot. Dead-slot
   cleanup can release an owner with no recovery-sensitive shared state. If MDL,
   transaction, read-view, InnoDB lock, or redo-visibility state remains for a
   dead owner while another process is live, cleanup is blocked and open returns
   busy until the durable recovery/rebuild path can run without live peers.
7. Add shared-memory rebuild from durable metadata and empty coordination logs.
8. Add capability probing for mmap visibility, byte-range lock behavior,
   release-on-death, remap after growth, and wait/wake behavior. The primitive
   evidence exists in tests and is summarized by an internal ownerless platform
   probe. `MYLITE_OPEN_OWNERLESS_RW` now uses the product ownerless startup path
   in normal embedded builds; unsupported surfaces remain tracked explicitly in
   the compatibility matrix.
9. Add crash tests for opener death, stale shared memory, process-slot reuse,
   resize interruption, recovery lock handoff, and waiters surviving missed
   wakeups.
10. Move ownerless read/write coverage into normal embedded builds while
    keeping deterministic fault injection and negative-proof bypasses in the
    unsafe test preset.

Exit criteria:

- Shared coordination files are rebuildable and crash-safe.
- The `.shm` format is stable enough to build later MDL, transaction, lock,
  and page-version segments without changing the lifecycle model.
- Linux has the intended high-performance wait path; other platforms either
  pass the probe or fail ownerless mode explicitly.
- No SQL write concurrency yet.

Detailed task list:

1. Shared-memory ABI:
   - define `mylite_shm_header`,
   - define segment descriptors,
   - define stable offset and ID helpers,
   - define shared atomic-word helpers with acquire/release memory ordering,
   - add format validation and compatibility errors,
   - add endian, word-size, page-size, and feature-flag checks.
2. File lifecycle:
   - create `concurrency/` with strict permissions,
   - create `.meta`, `.lock`, `.shm`, `.wal`, and `.ckpt`,
   - fsync parent directories after durable file creation,
   - grow `.shm` with `ftruncate()` under `SHM_RESIZE`,
   - prohibit shrink while active,
   - support remap on generation change.
3. Locking:
   - implement byte-range lock helper with named lock ranges,
   - decide Linux OFD lock use versus classic `fcntl` locks and document the
     close semantics,
   - define lock ordering in code and tests,
   - add debug assertions for lock-order violations,
   - keep the current exclusive `mylite.lock` until every replacement path has
     coverage.
4. Wait backend:
   - add fixed-width MyLite latch words,
   - implement uncontended atomic fast path,
   - implement Linux futex wait/wake,
   - implement timeout and cancellation,
   - implement portable fallback with byte-range locks plus adaptive backoff,
   - add metrics for spins, sleeps, wakes, timeouts, and owner death.
5. Process registry:
   - allocate slots under `OPEN_REGISTRY`,
   - publish slot generation and state transitions,
   - add liveness checks with PID, boot/start evidence, and generation,
   - release normal-owner MDL and lock-table entries before slot reuse,
   - clean dead slots only when no recovery-sensitive shared state remains or
     after durable recovery rules decide that cleanup is safe,
   - make missed cleanup safe and idempotent.
6. Recovery and rebuild:
   - detect dirty/rebuilding/stale generations,
   - acquire `RECOVERY`,
   - rebuild volatile segments from durable logs and checkpoints,
   - publish a new recovery generation,
   - wake or force-rescan waiters,
   - prove crashes at every state transition.
7. Capacity management:
   - start with fixed segment sizes for the first prototype,
   - add explicit capacity errors,
   - then add grow-only segment expansion,
   - benchmark remap cost and contention.
8. Observability:
   - expose internal test-only counters,
   - log recovery decisions in diagnostics,
   - record ownerless capability probe results,
   - add stress-test summaries for latch contention and rebuild time.

### Phase 5: Cross-Process MDL

Tasks:

1. Implement shared MDL map for schema/table-level locks.
   The current code has a fixed shared-memory MDL lock-table foundation segment
   and an internal cross-process metadata lock-table primitive with compatible
   shared holders, blocking exclusive conflicts, repeated-owner reference
   counts, same-owner mode upgrades, release wakeup, timeout coverage,
   MariaDB-style granted compatibility for schema IX/S/X locks, and
   MariaDB-style granted compatibility for table S/SH/SR/SW/SU/SRO/SNW/SNRW/X
   locks. It also has stable ownerless schema/table key hashing shaped after
   MariaDB's namespace/database/name MDL key structure. Product opens route
   MariaDB schema/table MDL through this segment using the runtime process-slot
   owner while the exclusive directory lock is still held. It does not model
   waiting-priority or cross-process MDL deadlock semantics yet.
2. Replace or wrap process-global `MDL_map` operations for MyLite ownerless
   mode. The current MariaDB patch adds a MyLite hook surface on the embedded
   MDL ticket lifecycle and covers balanced acquire/release events for
   schema/table tickets, including cloned tickets, upgrades, and downgrades.
   `libmylite` registers that hook against the directory-backed MDL lock-table
   segment with the current runtime process-slot owner. The lock-table segment
   is sized for multi-process statements that hold several schema/table tickets
   at once, including bounded multi-object read/write stress.
3. Add cross-process DDL/DML blocking tests:
   - `ALTER TABLE` waits for active `SELECT ... FOR UPDATE`,
   - `DROP TABLE` waits for active transaction,
   - concurrent DDL deadlock/timeout behavior.

Exit criteria:

- SQL metadata operations coordinate across processes.

### Phase 6: Cross-Process Transaction Visibility

Tasks:

1. Move transaction ID allocation to shared coordination state.
   The current code has an internal transaction-registry primitive in the
   production `.shm` layout that allocates monotonically increasing transaction
   IDs across parent and child mappings. InnoDB now has a guarded hook surface
   at `trx_sys_t::get_max_trx_id()`, `trx_sys_t::get_new_trx_id()`, and
   `trx_sys_t::register_rw()`. Normal persistent product opens register the
   shared transaction registry while the exclusive directory lock remains in
   place.
2. Move active read-write transaction visibility to shared state.
   The primitive can snapshot active transaction IDs in sorted order, report
   the next transaction ID for future `ReadViewBase::m_low_limit_id`, and track
   transaction serialisation numbers for purge-limit design. InnoDB now has a
   guarded hook surface at `trx_sys_t::snapshot_ids()`, and normal persistent
   product opens register the shared transaction registry while the exclusive
   directory lock remains in place.
3. Make read views include active transactions from every process.
   InnoDB `ReadView` creation now uses the shared transaction registry for the
   active transaction-ID snapshot under normal persistent product opens.
4. Add purge oldest-view coordination.
   The production `.shm` layout now has a read-view registry segment. InnoDB
   publishes read views before making them locally visible to purge, removes
   them on close, and merges the directory-owned oldest read view during purge
   oldest-view cloning. The registry is still fixed-capacity and remains behind
   the exclusive product lock until the later record-lock, page-visibility,
   redo/checkpoint, and recovery phases are complete. Ownerless runtime hooks
   now let purge free old undo history after refreshing rollback-segment
   metadata from the directory-visible header. Rollback-segment history commits
   also refresh and serialize the current first history-list undo page before
   prepending a new undo log, so the file-list splice does not read stale links
   from another process's buffer pool. Physical undo tablespace truncation stays
   disabled until that path has directory-owned rollback-segment metadata
   rebuild coverage.
5. Add crash cleanup for active transactions from dead process slots.
   Product opens now detect dead owners with active transaction/read-view/lock
   state and preserve that state while live peers remain. A guarded
   cross-process SQL test kills an uncommitted ownerless writer, verifies that a
   concurrent opener receives busy while another ownerless peer is live, then
   verifies that a no-live-process reopen rebuilds volatile shared state and
   sees only committed rows. Durable rollback/recovery records are still needed
   before product writers can recover a crashed owner while other processes
   continue running.

Exit criteria:

- Cross-process MVCC snapshots are correct for read-only transactions over
  stable committed data. Ownerless page-version visibility now pins the first
  read LSN for repeatable-read and serializable transactions, while
  read-committed transactions observe a later peer commit on the next
  consistent read. Ownerless `READ UNCOMMITTED` isolation requests are rejected
  until a cross-process dirty-read protocol exists, and isolation system
  variable assignments are rejected until they can be tracked without bypassing
  ownerless snapshot policy.
- Ownerless read/write opens remain disabled; this phase proves shared
  transaction visibility over controlled stable data and synthetic crash
  cleanup before real cross-process writers publish user data.

### Phase 7: Cross-Process InnoDB Lock Manager

Tasks:

1. Represent lock owners and waiters by stable IDs, not pointers.
   The ownerless InnoDB lock-registry primitive now represents table and record
   lock owners by process-slot owner ID plus a stable lock transaction ID. That
   ID is the MariaDB transaction ID when available, or a MyLite transient
   lock identity for locks acquired before `trx_t::id` exists. Lock resources
   use table/index/page/heap identifiers instead of process-local lock pointers.
2. Mirror or move InnoDB record/table lock queues into shared memory.
   The primitive covers MariaDB-compatible table and record conflict rules in
   direct shared-memory tests. InnoDB now has a guarded hook bridge for table
   lock creation/removal, record bitmap bit set/reset, waiting-lock grant,
   record-lock dequeue, local wait enqueue/reset, and discard paths. Product
   opens register table/record locks against the directory-backed InnoDB
   lock-registry segment while the exclusive directory lock remains in place.
   The registry now stores directory-owned wait edges, local InnoDB waits
   publish and clear those edges under embedded SQL coverage, and the primitive
   detects cross-process wait cycles. InnoDB table and record grant paths now
   perform a nonblocking shared-registry reservation before granting a native
   lock. The shared registry ignores conflicts within the same owner process
   because those transactions share one native InnoDB lock manager and buffer
   pool; local waits are still mirrored by the explicit InnoDB wait hook. On
   external conflicts, InnoDB now creates a local waiting lock, publishes a
   directory-owned wait edge, sleeps on the mapped wait word without holding
   local InnoDB latches, refreshes redo visibility after wake, and retries the
   native grant. Insert-intention checks that do not normally create a granted
   native lock now probe the shared registry before inserting so peer
   gap/next-key locks can block and time out with MariaDB error 1205.
   Ownerless write commits now flush dirty pages through the
   transaction commit LSN before releasing shared lock-registry entries, which
   avoids the previous whole-buffer-pool sync while still keeping the current
   visibility bridge conservative. Because the current implementation still
   uses one InnoDB buffer pool per process, the shared registry still has a
   page-level physical X resource for native lock records that have no record,
   gap, insert-intention, or supremum flags. Ordinary `REC_NOT_GAP` row locks
   keep their record identity, which avoids turning row-heavy transactions into
   broad same-page waits. A separate
   page-write lock-registry segment mirrors X/SX page-latch write ownership
   for B-tree and external-value pages with synthetic page-write resources so
   internal data page writes that do not surface as row locks still serialize
   across process-local buffer pools without being starved by row-lock-heavy
   transactions. Undo segment creation explicitly enters the
   ownerless tablespace-allocation write resource before reading
   rollback-segment slots or native free-space metadata, and holds it through
   the mini-transaction that creates the segment. The current correctness bridge
   flushes dirty pages for the undo tablespace before releasing that allocation
   resource, so a peer cannot reuse stale native undo free-space metadata.
   The production `.shm` layout also includes an ownerless AUTO_INCREMENT
   registry. InnoDB's default simple-insert path normally reserves values under
   a process-local `dict_table_t::autoinc_mutex`; ownerless mode now acquires a
   shared `LOCK_AUTO_INC`-compatible registry entry before reading that local
   counter, seeds or refreshes the local counter from the shared
   table-ID-keyed high watermark, and publishes the next available value before
   releasing the local mutex. Ownerless DDL coverage now raises and then lowers
   `ALTER TABLE ... AUTO_INCREMENT` from one process while an already-open peer
   inserts implicit IDs, proving the peer refresh path and high-watermark
   registry do not reuse values before or after forced `.shm` rebuild.
   Traditional native `LOCK_AUTO_INC` table locks
   continue to mirror through the shared InnoDB lock registry.
   Ownerless embedded waits use the current SQL thread's session lock-wait
   timeout if the InnoDB transaction is not linked to `trx->mysql_thd`. Normal
   embedded builds exercise this path through `MYLITE_OPEN_OWNERLESS_RW`
   instead of the raw directory-lock bypass environment variable.
3. Add cross-process wait/wakeup/deadlock detection.
   The lock registry stores wait edges by stable owner and transaction IDs,
   wakes waiters when active slots are released, wakes waiters on a
   transaction's held slots when that transaction publishes a new wait edge, and
   rechecks the wait graph before returning a lock-wait timeout. A final
   timeout path also rechecks whether the blocker has disappeared so a missed
   wake at the deadline can still grant the survivor instead of returning a
   stale timeout. This prevents cross-process cycles from degrading into
   timeout-only behavior. The same final availability recheck now applies to
   wait-only registry probes used by page-write wait paths before retrying the
   actual acquisition, with primitive coverage that clears a blocker without
   notifying the wait word and requires the waiter to succeed after the timeout
   recheck. Primitive coverage also kills a process while it has a live
   incompatible table-lock wait entry, verifies the shared waiting entry
   remains observable after process death, and requires owner cleanup to remove
   it.
4. Add timeout and victim-selection tests.
   Guarded SQL tests now cover non-conflicting writers, same-page writer
   serialization, same-row writer waits, savepoint rollback visibility before
   and after commit, serializable read locks blocking a peer writer,
   a two-process serializable write-skew candidate where both transactions
   cannot commit disjoint predicate-dependent updates,
   reverse-order table deadlocks, stale committed reads after an external write,
   mixed reader/writer processes, a bounded independent-table writer/reader
   stress loop, shared AUTO_INCREMENT assignment across concurrently opened
   ownerless insert workers, ownerless AUTO_INCREMENT DDL high-watermark
   refresh, consistent-snapshot retention without a preceding
   read, session-scoped and transaction-scoped read-committed visibility,
   cleanup of wait state after timeout/deadlock, and shared read-only handles
   observing an ownerless writer commit while rejecting writes through the
   read-only handle.

Exit criteria:

- Conflicting cross-process writers block and deadlock like InnoDB writers in
  one process.
- Ownerless read/write opens support the tested InnoDB conflict subset through
  `MYLITE_OPEN_OWNERLESS_RW`; broader page visibility, redo, DDL, purge, and
  recovery behavior remains phase-gated.

### Phase 8: Page Visibility Prototype

Tasks:

1. Build an experimental page-version log below InnoDB page IO.
   The current first-party primitive defines a fixed-header page-version log
   record format, serializes appends with a directory-file byte-range lock,
   supports `space_id=0` and `page_no=0` for real InnoDB identifiers, reads
   the newest version visible at or below a caller-supplied commit LSN, and
   tolerates an incomplete tail record left by an interrupted append. Primitive
   tests cover same-process visibility, too-small read buffers, missing pages,
   payload offsets, direct record-offset reads, and cross-process append
   serialization. Production
   ownerless runtimes initialize that primitive after the fixed
   `mylite-concurrency.wal` recovery header. InnoDB guarded commit flush now
   scans dirty buffer-pool pages up to the transaction commit LSN, formats page
   images with write-path checksums, and appends them before the conservative
   flush runs.
2. Keep page-version state available for rebuild/checkpoint evidence, but leave
   SQL page reads on the conservative native bridge for now.
   Guarded ownerless runtimes add a directory-backed page-version index segment
   to `mylite-concurrency.shm`; commit-page publishing caches the newest WAL
   record offset per `(space_id, page_no)`, and `.shm` rebuilds replay durable
   page-version WAL record metadata into the shared page-version index, so the
   index is no longer only live volatile state. The page-version index
   currently has 16,384 entries, and its shared-memory segment version changes
   when that capacity or layout changes so stale `.shm` files are rebuilt.
   Older snapshot lookups whose needed page version is no longer the cached
   newest entry fall back to the WAL scan. Product ownerless opens add a
   shared page-version pin registry for explicit repeatable-read and
   serializable snapshot LSNs. `START TRANSACTION WITH CONSISTENT SNAPSHOT`
   publishes its page-version pin before executing the SQL so close-time
   reclamation cannot race the native snapshot boundary. Close-time page-log
   reclamation can run with live peers when that registry reports zero active
   pins, and can now run with active pins when the page-version WAL proves every
   snapshot-sensitive data page advanced past the oldest pinned read LSN has a
   retained boundary record at or below that pin. When exactly one active pin is
   present, product reclaim uses the single-snapshot path and drops checkpointed
   post-snapshot records after native checkpoint proof; with multiple active
   pins it keeps the multi-pin path so later readers retain intermediate
   versions. The primitive invokes the
   native-checkpoint prepare callback only after that proof; if proof is
   missing, close leaves the WAL and page index unchanged. Undo, allocation,
   tablespace-header, extent, transaction-system, change-buffer, and system page
   records do not require oldest-snapshot boundary images because they are
   checkpointed as latest native MVCC/recovery support state, so safe records
   for those page types are dropped during active-pin reclaim. Unrecognized page
   types remain snapshot-sensitive. Dead-owner cleanup releases a killed
   reader's MDL, read-view, and page-version pin state so it does not starve
   later live-peer reclamation. No-live-process
   recovery applies visible page-version records into
   native tablespace files and retains complete committed WAL records until
   native redo/checkpoint
   reconciliation can prove that record reclamation is safe. Guarded ownerless
   SQL allows page-version reads for direct or prepared `SELECT`/`WITH`
   statements at a live page-version read LSN, including transactions with
   local writes whose own uncommitted redo can hold back the durable
   page-visible LSN. Repeatable
   read and serializable transactions pin that live read LSN on their first
   consistent read, and `START TRANSACTION WITH CONSISTENT SNAPSHOT` pins it at
   transaction start. Active transactions that cannot safely run a global
   refresh keep dirty local pages resident, and clean-page refresh skips locally
   dirty buffer pages. InnoDB read completion
   validates ownerless page identity and checksum in a temporary buffer and
   overlays the disk frame only when the disk frame is invalid for the expected
   page or older by page LSN. After InnoDB startup completes, this includes
   `space_id=0` system-tablespace pages so dictionary flushes after local or
   peer DDL can reload unflushed dictionary records from the page-version WAL;
   startup and recovery keep this hook disabled while redo/log initialization
   is still in progress. No-live-process
   recovery treats the page-version WAL as the visibility authority and applies
   the latest visible page-version record by the same commit-first ordering as
   page-version reads to existing native InnoDB tablespace files before
   rebuilding `.shm`, even when disk carries a higher-LSN page image left by a
   crashed uncommitted writer. It uses page-0 space-id discovery for existing
   single-file tablespaces. Product no-live recovery uses an explicit replay
   mode to skip retained page-version records for tablespaces that are no
   longer present, such as dropped DDL stress tables, while the strict primitive
   replay API still fails closed on unresolved tablespaces. Broader DML/DDL and
   DDL-created tablespace replay still use the conservative native-file bridge
   until the page replay protocol carries durable file lifecycle metadata. The
   conservative bridge now advances
   the local durable LSN when a process reads an externally flushed page whose
   page LSN is ahead of the local log, and refreshes durable tablespace header
   and allocation metadata from page 0 plus the file-segment inode page after
   visible peer commits so native allocation bounds do not remain stale.
3. Publish commit end marks and reader snapshots.
   Guarded commits now separate raw redo progress from page-visible progress in
   the ownerless redo state segment. `redo_leave` still advances the raw latest
   LSN used to keep peer InnoDB redo state monotonic, but the page-visible LSN
   advances only after dirty pages up to that commit LSN have been published
   into the page-version log, the page-version log has been fsynced under the
   append range, and those pages have been flushed through the current
   conservative native bridge. Page-version WAL lookups capture a stable
   log-end snapshot under the append lock and release that lock before
   scanning, so rebuild and checkpoint paths see one immutable WAL prefix
   without blocking concurrent appends for the full scan. Ownerless statement
   startup advances the local InnoDB redo state to the maximum of the shared
   raw latest LSN and page-visible LSN for autocommit statements, including
   repeated autocommit DML on one connection, but not inside explicit
   transactions or active `autocommit=0` transactions; active writer
   transactions must not globally flush or evict their own dirty pages. The
   SQL-level ownerless statement locks use a runtime-owned descriptor for
   `mylite-statements.lock` so ordinary statement locking does not reopen the
   same byte-range lock file on every statement.
   External record waits use targeted waited-page refresh after the blocker
   releases. Page-version scans, rebuilds, and checkpoints ignore only the
   final incomplete or checksum-corrupt tail record; checksum failure before
   the tail is treated as corruption.
   Undo and system-page versions publish at mini-transaction scope after
   ownerless redo is written and the raw latest LSN is recorded, because
   cross-process MVCC readers may need active transactions' undo pages to build
   previous row versions before the writer commits. User data/index pages remain
   transaction-visible and are published only at commit/rollback visibility
   boundaries. Ownerless mini-transactions therefore make pre-write preparation
   page-kind aware: once an explicit transaction has deferred user page writes,
   later undo and system-page writes still acquire ownerless page-write
   ownership before page-linked state is read or modified.
   Deferred ownerless page-write locks must never continue after a dirty
   deadlock without owning the directory-backed page-write resource. Guarded
   dirty-page paths therefore retry dirty page-write deadlocks instead of
   returning unlocked, and ownerless page-write ownership is acquired only when
   a persistent page becomes dirty, not for every X/SX page latch. Mini-
   transaction-local acquisitions are tracked separately so `MTR_LOG_NONE`
   paths release transient page-write locks even when no `MTR_MEMO_MODIFY`
   memo remains. Rollback-segment history commit waits also treat shared
   page-write deadlock reports as retryable physical-page waits because that
   commit serialization path has no safe SQL error return once native commit is
   in progress. Explicit transaction handler write locks take a
   transaction-level page-write gate before the table is counted in the
   statement; the first table uses a tablespace-scoped gate to preserve
   independent-table writer concurrency, and later tables in the same statement
   use a global gate until the physical-page bridge can merge concurrent page
   images more finely. Explicit transaction statement-end cleanup releases only
   those transaction gate markers while leaving real dirty page-write locks
   held until commit or rollback.
4. Implement passive checkpoint of safe page versions into tablespace files.
   The page-version log primitive can now compact away records at or below a
   safe commit LSN, retain newer records at new offsets, and report those
   retained offsets through the replay callback shape used by shared-index
   rebuild. Product close-time reclamation now forces native checkpoint
   evidence before compacting complete retained records at or below the durable
   visible LSN, retains newer records, replaces the shared page-version index
   under checkpoint locks, and may run with live peers only when no active
   shared page-version snapshot pins exist. Recovery applies the latest visible
   page-version image per
   `(space_id, page_no)` to existing native tablespace files using the page-log
   latest-visible rule: highest visible commit LSN first, then page LSN as the
   tiebreaker. It trims incomplete or corrupt page-log tails, retains complete
   committed records until native checkpoint proof allows no-peer reclamation,
   and rebuilds the shared page-version index from retained records. Scans and
   direct record reads take a checkpoint read lock, and indexed direct-offset
   reads verify the
   retained record's `(space_id, page_no)` before using its page image, while
   compaction/truncation takes the checkpoint write lock plus the append lock.
   Primitive coverage now includes a same-page replay history where the newer
   visible commit has a lower page LSN, proving tablespace replay does not pick
   a stale commit only because its page LSN is higher. Tablespace replay treats
   the latest visible WAL image as authoritative during no-live-process
   recovery: if the resolved disk page has different bytes, including the same
   LSN from another process-local redo history or a newer LSN from a killed
   uncommitted writer, replay rewrites it to the visible WAL image. Strict
   primitive replay fails closed when the tablespace cannot be resolved;
   product no-live recovery uses an explicit mode to skip unresolved retained
   records for tablespaces no longer present in the directory.
5. Run kill tests around write, commit publish, checkpoint, and recovery.
   Existing guarded SQL coverage kills an uncommitted ownerless writer and
   verifies live-peer cleanup behavior: live peers release the dead owner's
   page-write locks but preserve transaction, redo, lock, and page-version
   recovery evidence until no-live-process recovery replays the visible WAL
   state. Deterministic unsafe-test faults now pause before page-version WAL
   append, after page-version WAL append but before shared-index publication,
   after volatile `.shm` page-visible publication but before `.ckpt`
   persistence, after the page-visible LSN is durably checkpointed, and
   immediately before
   no-live-process recovery checkpoint truncation; the cross-process SQL suite
   kills those processes, reopens the directory, and verifies data remains
   readable after both normal dirty-`.shm` recovery and forced `.shm`
   recreation. The pre-`.ckpt` page-visible fault also asserts that the
   durable checkpoint visible LSN did not advance while the volatile redo
   segment did, so the passing reopen cannot depend on `.shm` as durable truth.

Exit criteria:

- A process can read committed pages written by another process without waiting
  for all pages to be flushed directly to tablespace files.

### Phase 9: Cross-Process Redo And Commit

Tasks:

1. Globalize LSN allocation.
   The current guarded path serializes local InnoDB redo writes under the
   directory-owned redo latch and publishes the latest raw LSN plus
   page-visible LSN in `.shm`. The `.ckpt` anchor now persists those LSNs, and
   rebuilt `.shm` redo state plus clean runtime attach are seeded
   monotonically from that durable record, so shared-memory rebuild or stale
   clean shared memory cannot reset peer redo/page-visibility progress to zero.
   Page-visible publication now first fsyncs the page-version WAL under a
   safe serialized sync point. The redo segment bookkeeping now lives in a
   first-party primitive that owns latch/refcount handling, latest/visible LSN
   publication, reserved-LSN counters, contiguous written-LSN tracking,
   coalescing for out-of-order completed ranges, snapshot reads, and dead-owner
   cleanup; the InnoDB mini-transaction append path now reserves its redo byte
   range from that shared state, advances the local append cursor to the
   directory-owned reservation, fails closed if MariaDB's local append range does
   not match that reservation, and reports the completed write range after the
   local redo write. Refresh-only paths observe the latest shared redo LSN
   without entering a serialized redo latch. Redo writer entry/leave now uses
   short active-entry records instead of a long-lived global latch, and active
   reservation slots keep in-flight ranges recovery-sensitive. The redo state
   segment is sized as one page with headroom beyond the current process-slot
   count, and primitive coverage reserves 32 active ranges before completing
   them. The short redo progress latch is also surfaced in snapshots so cleanup
   treats a dead owner inside reservation or completion bookkeeping as
   recovery-sensitive instead of clearing the process slot.
2. Relax serialized redo append into concurrent atomic reservations.
   The current append-range and written-range hooks atomically reserve disjoint
   redo ranges and track contiguous write completion. Page-visible publication
   is clamped to the contiguous written LSN, so a later writer cannot expose
   pages past an earlier unwritten redo gap. The current product behavior is
   safe serialized commit, not group commit: peers may wait behind an unwritten
   redo gap, and group commit remains a future optimization candidate. This
   safe-serialization claim stays bounded to covered SQL, fault, and stress
   paths until native checkpoint reclamation and external oracle stress are
   added.
3. Define group commit or safe serialized commit.
   The current ownerless support posture is safe serialization when a redo gap
   exists: unsafe-hook SQL coverage pauses one writer after reserving redo, then
   proves a later writer on another table remains blocked instead of committing
   past the unwritten gap. After both interrupted writers are killed, no-live
   recovery rebuilds volatile coordination and preserves only the previously
   committed baseline rows. Normal SQL coverage now also starts multiple
   explicit ownerless transactions on independent tables, pauses them after their
   writes and before commit, releases all commits together, and verifies every
   committed delta is durable and visible through the live ownerless runtime and
   after forced `.shm` rebuild. The opt-in transaction stress coverage exercises
   the same commit path with repeated savepoint rollback and concurrent
   rollback-segment history page-write waits. The concurrent commit race now
   also asserts that committed page-version WAL records exist or have been
   checkpointed, that the durable page-visible checkpoint advances, and that a
   forced `.shm` rebuild seeds redo-visible state from that durable checkpoint.
   Cross-process group commit remains an optimization candidate rather than
   claimed behavior.
4. Reconcile InnoDB redo with MyLite page-version visibility.
   Tablespace replay now treats the page-version WAL image as authoritative even
   when an existing native page has the same page LSN: equal page LSNs can come
   from independent process-local redo histories, so replay skips only when the
   full disk page already matches the selected WAL image. Primitive coverage
   rewrites a same-LSN different-image page. Ordinary native exclusive
   read/write opens now keep page-version reads enabled and no-live-process
   replay retains complete page-version WAL records, so covered concurrent
   explicit ownerless commits remain visible through `MYLITE_OPEN_READWRITE`
   before and after forced `.shm` rebuild. Product no-live replay also skips
   retained page-version records whose tablespace no longer exists, covering
   dropped DDL stress tables without treating stale `.shm` state as durable
   truth. Native InnoDB redo/checkpoint reconciliation is still incomplete:
   MyLite now reclaims retained page-version records on non-read-only runtime
   close after forcing a native InnoDB checkpoint, advancing local native LSN
   state to the durable page-visible LSN when needed, refreshing external clean
   page state before checkpoint proof, proving the native checkpoint
   covers that durable visible LSN according to MariaDB's checkpoint-record
   rule, compacting records at or below that safe LSN while retaining newer
   complete records, and replacing the page-version index before checkpoint
   locks are released. With live peers, this path is gated by the shared
   page-version pin registry and runs with active pins only after data-page
   boundary proof. Page-version publication now opportunistically synthesizes a
   boundary record from the native tablespace page when an older snapshot pin is
   active, no WAL boundary exists, and the native page LSN is at or below the
   oldest pin; if that proof is unavailable, missing data-page boundaries still
   conservatively leave the WAL unchanged. Product close-time reclaim now uses
   the single-active-pin page-log primitive when the registry snapshot reports
   exactly one active pin. Bounded SQL-level repeated same-row pressure and
   distinct large-row expanding-page pressure are now covered while a live
   repeatable-read snapshot pin remains active; broader user-visible pressure
   policies remain planned.
   The `ownerless-native-checkpoint-reclamation`,
   `ownerless-partial-page-log-reclamation`,
   `ownerless-live-reclaim-gating`, `ownerless-active-pin-reclaim`,
   `ownerless-native-boundary-synthesis`, and
   `ownerless-active-reader-pressure` slices record the source-backed
   boundaries for that reclamation work. The
   `ownerless-expanding-page-pressure` slice adds bounded SQL evidence for
   distinct large-row page sets under the same active-reader pin policy.
5. Add power-fail style crash tests with fault injection.
   The current unsafe-hook SQL coverage kills a writer before page-version WAL
   append and after page-version WAL append but before shared-index
   publication, then verifies a subsequent ownerless writer can proceed and a
   forced `.shm` rebuild remains readable.
   Transaction-registration fault coverage kills a writer after the shared
   transaction registry begins a read-write transaction but before the update
   proceeds; live-peer cleanup must stay busy while that active transaction
   entry is present, and no-live reopen must rebuild volatile coordination
   without applying the interrupted update.
   Record-lock grant fault coverage blocks a writer behind a peer row lock,
   releases the holder, kills the writer after MariaDB grants the local wait
   lock and MyLite publishes that record lock to shared state, then proves
   live-peer cleanup stays busy and no-live reopen preserves only committed
   peer updates.
   Record-lock before-grant fault coverage kills a writer after it enters the
   external ownerless record wait but before MariaDB grants the local waiting
   record lock; live-peer cleanup must stay busy while the interrupted writer
   state exists, and no-live reopen must preserve only committed data.
   Redo reservation fault coverage kills a writer after the directory-owned
   redo range is reserved but before local redo bytes are appended; live-peer
   cleanup must stay busy while the dirty reservation is present, and no-live
   reopen must rebuild volatile coordination without applying the interrupted
   update. Redo-gap serialization coverage holds that reserved gap open and
   proves a later writer cannot commit past it. Redo completed-write fault
   coverage kills a writer after the shared
   redo segment marks the reserved bytes written but before `redo_leave`
   publishes the latest LSN to `.ckpt`; live-peer cleanup must still stay busy,
   no-live reopen must rebuild volatile coordination without applying the
   interrupted update, and the test asserts the volatile written LSN advanced
   while the checkpoint latest LSN did not. Redo latest-checkpoint fault
   coverage kills a writer after `redo_leave` advances the volatile raw latest
   LSN but before `.ckpt` persistence, then verifies the durable checkpoint did
   not advance, live-peer cleanup remains recovery-sensitive, and no-live
   reopen does not apply the interrupted update. Redo latest-checkpoint-after
   fault coverage kills a writer after `.ckpt` latest LSN persistence but
   before page-visible publication, then verifies the visible checkpoint did
   not advance and no-live reopen still does not apply the interrupted update.
   Page-visible publish fault
   coverage kills a writer after native pages and the page-version WAL are
   flushed and the volatile `.shm` page-visible LSN advances, but before `.ckpt`
   persistence; normal reopen and forced `.shm` recreation must both preserve
   the committed update without trusting `.shm` as durable truth. Page-visible
   checkpoint fault coverage kills a writer after the
   committed page-visible LSN is persisted to `.ckpt`; normal reopen and a
   forced `.shm` rebuild must both preserve the committed update. Native
   checkpoint reclamation fault coverage kills a closing writer after the
   native checkpoint proof but before page-version WAL truncation, then verifies
   retained WAL recovery and later normal reclamation both preserve the
   committed update. A resumable native checkpoint reclamation fault also
   pauses a closing writer after native checkpoint proof, lets a peer commit a
   newer update, then verifies the older closer does not truncate past the newer
   complete page-version records and both updates survive ownerless and native
   exclusive reopen.

Exit criteria:

- Committed cross-process writes survive process crashes and full restart.

### Phase 10: DDL, Dictionary, And Space Allocation

Tasks:

1. Coordinate table ID and space ID allocation across processes.
   Current coverage starts multiple ownerless writers together, creates and
   alters separate InnoDB tables, verifies the parent process can see every
   table through an already-open handle, and checks
   `INFORMATION_SCHEMA.INNODB_SYS_TABLES` for unique final `TABLE_ID` and
   `SPACE` values. Those concurrent workers also add an online/in-place
   secondary index to every created table, replace it with an online/in-place
   drop-plus-add, and the already-open parent verifies both the removed and final
   `INFORMATION_SCHEMA.STATISTICS` rows. Standalone unique-index coverage
   creates and drops a multi-column unique index from another ownerless process,
   verifies an already-open peer observes `NON_UNIQUE = 0`, rejects duplicate
   writes while the index exists, and accepts the formerly duplicate key shape
   after the index is dropped. Primary-key replacement coverage now performs
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)` from another
   ownerless process, verifies an already-open peer observes `PRIMARY` on the
   replacement column, rejects a duplicate replacement-key write, accepts a
   duplicate of the old key column, and verifies the final clustered-index
   metadata through ownerless/native reopen before and after forced `.shm`
   rebuild. This proves the current dictionary-generation serialization and
   pre-statement refresh path for the representative create/alter/index
   allocation, replacement, unique-index enforcement, and primary-key
   replacement cases. AUTO_INCREMENT DDL coverage raises and then lowers the
   table option from one ownerless process while an already-open peer inserts
   implicit IDs, verifying peer-visible high-watermark refresh plus
   ownerless/native reopen before and after forced `.shm` rebuild.
   Secondary-index rename coverage now performs
   `ALTER TABLE ... RENAME INDEX` from another ownerless process, verifies an
   already-open peer observes the new index name while the old `FORCE INDEX`
   name fails, and verifies the final metadata through ownerless/native reopen
   before and after forced `.shm` rebuild.
   Ignored-index coverage now performs
   `ALTER TABLE ... ALTER INDEX ... IGNORED` and `NOT IGNORED` from another
   ownerless process, verifies an already-open peer observes the `IGNORED`
   metadata transitions, and verifies the final not-ignored index through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Peer-refresh coverage also exercises foreign-key table creation, foreign-key
   ALTER add/drop enforcement, foreign-key parent-table rename metadata and
   enforcement refresh, foreign-key child-table rename generated constraint
   refresh, cross-schema foreign-key parent-table and child-table rename
   refresh, same-schema foreign-key multi-pair parent/child rename refresh,
   cross-schema foreign-key multi-pair parent/child rename refresh,
   CHECK constraint ALTER add/drop enforcement, generated-column metadata,
   generated-column ALTER add/drop refresh,
   table-wide character-set conversion from `latin1` to `utf8mb4`,
   row-format rebuild from `COMPACT` to `DYNAMIC`,
   table comment metadata changes,
   `ALTER TABLE ... FORCE` rebuild,
   column-default SET/DROP metadata and peer DML effects,
   an online/in-place index alter variant,
   column-shape ALTERs that add, modify, rename, and drop columns,
   explicit InnoDB instant ADD/DROP/reorder column metadata,
   `CREATE TABLE ... LIKE`, and `CREATE TABLE ... SELECT`. The same broader DDL
   selector now closes all ownerless peers and verifies the final state through
   no-live ownerless read/write reopen, ordinary exclusive read/write reopen,
   forced `.shm` deletion plus ownerless rebuild, and ordinary exclusive reopen
   after that rebuild. Schema lifecycle coverage now creates a schema and InnoDB
   table from one ownerless process, verifies an already-open peer observes and
   writes through the new schema, drops the schema from the DDL process, and
   verifies peer-visible absence plus ownerless/native reopen before and after
   forced `.shm` rebuild. Cross-schema rename coverage now creates an InnoDB
   table in `app`, writes through it from an already-open peer, renames it into
   a second schema from the DDL process, verifies peer-visible source absence
   and target readability/writeability, checks `.frm` and `.ibd` movement
   between schema directories, and verifies final ownerless/native reopen before
   and after forced `.shm` rebuild. Multi-rename-cycle coverage now executes a
   three-pair `RENAME TABLE` swap in one statement, verifies an already-open
   peer sees row contents under the swapped names, checks that InnoDB `SPACE`
   identities swap with the table names, and verifies final ownerless/native
   reopen before and after forced `.shm` rebuild. View metadata coverage now
   creates and queries a simple view over an InnoDB base table from one
   ownerless process,
   verifies that an already-open peer observes the view and base-table changes
   through it, drops the view, and verifies final view absence plus base-table
   durability through ownerless/native reopen before and after forced `.shm`
   rebuild. Trigger
   metadata coverage now creates an InnoDB base/audit pair and an `AFTER INSERT`
   trigger from one ownerless process, verifies an already-open peer observes
   and fires the trigger, drops the trigger, and verifies later peer DML no
   longer fires it plus final trigger-file absence and base/audit durability
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Stored-routine DDL is a deliberately unsupported ownerless class for now:
   the routine path writes `mysql.proc`/`mysql.procs_priv` and a proof attempt
   hit a MariaDB error 145 `proc` system-table failure, so ownerless mode now
   rejects `CREATE`/`ALTER`/`DROP FUNCTION` and `PROCEDURE` before those
   uncoordinated metadata writes. Sequence SQL is also deliberately
   unsupported in ownerless mode: sequences are table-backed objects and
   `NEXT VALUE` / `NEXTVAL()` mutates sequence state, so ownerless mode rejects
   sequence DDL and value access until sequence-table coordination is designed.
   Table-admin SQL is also deliberately unsupported in ownerless mode:
   `ANALYZE TABLE`, `CHECK TABLE`, `CHECKSUM TABLE`, `OPTIMIZE TABLE`, and
   `REPAIR TABLE` enter MariaDB SQL admin handlers that can scan table pages
   outside the proven ownerless `SELECT` snapshot-read surface, update
   statistics, check upgrade state, repair files, or rebuild tables without
   going through MyLite's ownerless dictionary DDL generation boundary.
   SQL locked-table mode is also deliberately unsupported in ownerless mode:
   `LOCK TABLES` keeps connection-level table and handler locks alive until
   `UNLOCK TABLES`, and current evidence only covers primitive table-lock
   wait-entry cleanup rather than MariaDB's SQL locked-table lifecycle across
   processes.
   Ownerless `FLUSH TABLES ... WITH READ LOCK` and
   `FLUSH TABLES ... FOR EXPORT` are also rejected: MariaDB routes these forms
   through global read-lock, locked-table, InnoDB quiesce, and checkpoint
   disable/export paths that require a separate ownerless backup/export
   protocol. Plain ownerless `FLUSH TABLES` remains covered for local
   dictionary/table-cache refresh.
   Server thread-control SQL is rejected by the global MyLite server-surface
   policy: `KILL` targets server connection threads and `SHUTDOWN` targets
   daemon lifetime, while ownerless coordination uses directory-owned process
   slots and recovery state rather than SQL commands that control another
   embedded connection.
   `FULLTEXT` and `SPATIAL` index DDL is also rejected in ownerless mode until
   InnoDB full-text auxiliary state, spatial R-tree pages, spatial predicate
   locks, and special-index recovery are designed; current ownerless index
   coverage remains scoped to ordinary InnoDB secondary indexes.
   Partitioned table DDL is also rejected in ownerless mode until partition
   metadata, `.par` files, per-partition native engine files, partition
   maintenance, and partition-aware no-live replay are designed.
   Table `DATA DIRECTORY` and `INDEX DIRECTORY` options are rejected globally,
   including ownerless mode, because MariaDB can store native table data/index
   files or path metadata outside the MyLite database directory and MyLite has
   no external table-file lifecycle protocol.
   `ALTER TABLE ... DISCARD/IMPORT TABLESPACE` is rejected in ownerless mode
   until explicit tablespace detach/import file lifecycle metadata and recovery
   replay are designed.
   Unsafe-hook coverage kills a process
   after dictionary DDL is marked
   active but before MariaDB executes it, after successful DDL execution but
   before the ownerless dictionary generation is published stable, and after the
   generation is published stable but before the process returns. The tests
   verify recovery-sensitive active dictionary state blocks live-peer cleanup,
   no-live reopen rebuilds volatile coordination, completed DDL remains usable,
   and stable dictionary publication lets live peers proceed. Broader online DDL
   classes beyond the covered ordinary/unique index, secondary-index rename,
   ignored-index metadata, primary-key replacement, foreign-key ALTER,
   same-schema foreign-key parent-table/child-table rename, cross-schema
   foreign-key parent-table/child-table rename, same-schema foreign-key
   multi-pair parent/child rename, cross-schema foreign-key multi-pair
   parent/child rename, CHECK constraint ALTER, generated-column ALTER, table
   charset conversion, row-format rebuild, table comment metadata,
   `ALTER TABLE ... FORCE` rebuild, column-default SET/DROP, column-shape, and
   instant-column variants remain planned.
2. Coordinate create, drop, truncate, rename, and online DDL.
   The current ownerless SQL coverage exercises representative cross-process
   metadata-lock blocking by holding an InnoDB transaction in one process and
   verifying that `ALTER TABLE` in another process times out through the
   directory-backed MDL path, then succeeds after the holder releases. Current
   durable tablespace-header refresh is grow-only for native allocation fields
   and is enough for bounded writer stress, but broader DDL must add
   shrink/truncate invalidation instead of relying on grow-only header
   observation. Peer-refresh coverage now performs DML on an already-open peer
   handle immediately after another process truncates the table, proving the
   current dictionary/cache refresh path can reuse the truncated table before
   the peer proceeds to drop it. Additional coverage grows a larger InnoDB table,
   keeps the creating handle open, has a peer truncate it, then reuses the
   truncated table for large rows from the original handle so stale allocation
   bounds do not survive the peer truncate boundary. The same already-open peer
   then recreates and writes the dropped table name after another process drops
   it, covering same-name file/dictionary reuse across the peer DDL boundary.
   Opt-in stress coverage now runs concurrent create/insert/alter
   index/rename/truncate/drop workers while peer DML writers and a reader keep
   checking committed visibility on an existing InnoDB table.
3. Coordinate shared InnoDB temporary tablespace lifecycle.
   Ownerless read/write startup gives each process a private InnoDB temporary
   tablespace under its runtime `tmp/` directory and gates
   `srv_tmp_space.delete_files()` via the process registry, so a process does
   not remove a peer's temporary tablespace while that peer is active or
   opening. Ownerless SQL tracks connection-local temporary table names and
   avoids global page/dictionary refresh while statements reference those
   tables, preserving MariaDB temporary-table isolation. Cross-process SQL
   coverage now starts two
   ownerless peers that each hold a same-named InnoDB temporary table, verifies
   each peer's rows remain connection-local while another ownerless handle
   operates, kills one temporary-table peer while another remains live, verifies
   a new ownerless opener can still use its own same-named temporary table, and
   then verifies the name can be reused for a persistent InnoDB table after the
   temporary sessions are gone. Opt-in stress coverage now churns same-named
   InnoDB temporary tables across several ownerless processes and verifies the
   name can be reused for a durable table after the temporary sessions close.
4. Add dictionary generation invalidation in every process.
   The current ownerless runtime has a directory-backed odd/even dictionary
   generation. Ownerless DDL marks the generation active before execution and
   publishes the next even generation afterward. Peers wait for the generation
   to become stable, refresh external page visibility, run `FLUSH TABLES`, and
   evict unused InnoDB dictionary-cache entries before using the new metadata.
   Local DDL followed by an explicit dictionary/table flush is covered so the
   same handle can immediately read a just-created durable InnoDB table through
   system-tablespace page-version replay.
5. Add broad DDL compatibility tests.
   Current cross-process coverage verifies peer visibility after `ALTER TABLE`
   on an already-cached InnoDB table plus create, rename, truncate,
   post-truncate DML, large-table post-truncate allocation reuse, drop, and
   same-name recreate in another process, and concurrent create/alter/index
   workers verify unique InnoDB table, space, and secondary-index metadata
   allocation plus online index drop/replacement visibility. Additional
   peer-refresh coverage verifies foreign-key cascade behavior, CHECK
   constraint add/drop enforcement, generated-column recalculation,
   generated-column ALTER add/drop with stored and virtual generated
   expressions, `CREATE TABLE ... LIKE`, `CREATE TABLE ... SELECT`,
   and an online/in-place index alter plus column add/modify/rename/drop ALTERs
   and explicit instant ADD/DROP/reorder column metadata performed by another
   ownerless process. The broader DDL selector also verifies that the final
   altered/copy/instant table state survives no-live ownerless and native
   exclusive reopen before and after forced `.shm` rebuild. Schema lifecycle
   coverage adds ownerless `CREATE DATABASE` plus InnoDB table creation,
   peer-write visibility, `DROP DATABASE`, and absent-schema reopen checks.
   Cross-schema rename coverage adds ownerless `RENAME TABLE app.t TO other.t`,
   already-open peer metadata refresh for the old and new schema-qualified
   names, peer writes through the moved table, `.frm`/`.ibd` movement checks,
   and ownerless/native reopen checks before and after forced `.shm` rebuild.
   Multi-rename-cycle coverage adds a three-pair
   `RENAME TABLE left TO tmp, right TO left, tmp TO right` swap, already-open
   peer metadata refresh for the final names, InnoDB `SPACE` identity swap
   checks, peer writes through both swapped tables, and ownerless/native reopen
   checks before and after forced `.shm` rebuild.
   View metadata coverage adds ownerless `CREATE VIEW` over an InnoDB base
   table, peer-visible view queries, `DROP VIEW`, and absent-view reopen checks
   before and after forced `.shm` rebuild. Trigger metadata coverage adds
   ownerless `CREATE TRIGGER` over an InnoDB base table, peer-fired audit-table
   effects, `DROP TRIGGER`, and absent-trigger reopen checks before and after
   forced `.shm` rebuild. Standalone index DDL coverage adds ownerless
   `CREATE INDEX`/`DROP INDEX` over an InnoDB base table, already-open peer
   metadata refresh through `information_schema.statistics`, forced-index use
   before drop, and final absent-index checks before and after forced `.shm`
   rebuild. Secondary-index rename coverage adds ownerless
   `ALTER TABLE ... RENAME INDEX`, already-open peer metadata refresh for the
   old and new index names, forced-index rejection for the old name, forced-index
   use for the new name, and final renamed-index checks before and after forced
   `.shm` rebuild. Ignored-index coverage adds ownerless
   `ALTER TABLE ... ALTER INDEX ... IGNORED` and `NOT IGNORED`, already-open
   peer metadata refresh through `information_schema.statistics.IGNORED`, DML
   while the index is ignored, forced-index use after the index is restored, and
   final not-ignored index checks before and after forced `.shm` rebuild.
   Unique-index coverage adds multi-column `CREATE UNIQUE INDEX`,
   peer-visible `NON_UNIQUE = 0` metadata, duplicate-key enforcement before
   drop, duplicate-key insertion after drop, and final absent-index checks
   before and after forced `.shm` rebuild. Primary-key coverage adds
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)`, peer-visible
   `PRIMARY` metadata on the replacement column, duplicate-key enforcement on
   the new key, old-key duplicate insertion after replacement, and final
   replacement-primary-key checks before and after forced `.shm` rebuild.
   Foreign-key ALTER coverage adds a named child-to-parent foreign key from
   another ownerless process, verifies missing-parent rows fail while it exists,
   drops the foreign key, and verifies the formerly invalid child row shape can
   be inserted plus final absent-FK checks before and after forced `.shm`
   rebuild. The same coverage also verifies the child FK secondary-index pages
   rebuilt by ALTER are published through the ownerless DDL dirty-page path
   before peers observe the dictionary generation, while ordinary DML commits
   remain on the transaction-page vector path so MVCC undo-history pages are
   not exposed through broad flush-list publication.
   Foreign-key action coverage now keeps an already-open ownerless peer active
   while another process performs `ON UPDATE CASCADE`, `ON DELETE CASCADE`,
   and `ON DELETE SET NULL` parent-row changes, verifies the peer sees the
   cascaded child-row updates/deletes and set-null child key, verifies inserts
   against the old parent key fail with MariaDB errno 1452, verifies
   `ON DELETE RESTRICT` fails with errno 1451, and checks the final state
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Composite foreign-key coverage now uses a tenant-scoped parent primary key
   `(tenant_id, id)` and child foreign key `(tenant_id, parent_id)`, verifies
   missing composite-parent enforcement, cascades an update for only one
   tenant's shared numeric key component, proves another tenant's `(2, 10)`
   child rows remain attached to that parent, verifies composite
   `ON DELETE RESTRICT` returns errno 1451, and checks ownerless/native reopen
   before and after forced `.shm` rebuild. Parent-table foreign-key rename
   coverage now renames a referenced parent table from another ownerless
   process, verifies an already-open peer observes
   `REFERENTIAL_CONSTRAINTS` move from the old parent name to the new name,
   checks `.frm` and `.ibd` movement, inserts a valid child row through the
   renamed parent, rejects a missing-parent child insert with errno 1452,
   rejects deleting a still-referenced parent with errno 1451, and checks
   ownerless/native reopen before and after forced `.shm` rebuild. Child-table
   foreign-key rename coverage now renames the child table that owns an unnamed
   foreign key, verifies an already-open peer observes the generated
   `<child>_ibfk_1` constraint name and `REFERENTIAL_CONSTRAINTS.TABLE_NAME`
   move from the old child table to the new child table, checks `.frm` and
   `.ibd` movement, inserts a valid child row through the renamed child table,
   rejects a missing-parent child insert with errno 1452, rejects deleting a
   still-referenced parent with errno 1451, and checks ownerless/native reopen
   before and after forced `.shm` rebuild. Cross-schema foreign-key parent-table
   rename coverage now moves a referenced parent table into another schema,
   verifies an already-open peer observes `UNIQUE_CONSTRAINT_SCHEMA` and
   `REFERENCED_TABLE_NAME` move to the target schema/table, checks `.frm` and
   `.ibd` movement between schema directories, inserts a valid child row through
   the moved parent, rejects a missing-parent child insert with errno 1452,
   rejects deleting a still-referenced moved parent with errno 1451, and checks
   ownerless/native reopen before and after forced `.shm` rebuild.
   Cross-schema child-table foreign-key rename coverage now moves the table
   owning an unnamed foreign key into another schema, verifies an already-open
   peer observes `CONSTRAINT_SCHEMA`, generated `<child>_ibfk_1` constraint
   identity, and `TABLE_NAME` move to the target schema/table while the parent
   remains in `app`, checks `.frm` and `.ibd` movement between schema
   directories, inserts a valid child row through the moved child, rejects a
   missing-parent child insert with errno 1452, rejects deleting a
   still-referenced parent with errno 1451, and checks ownerless/native reopen
   before and after forced `.shm` rebuild. Foreign-key multi-pair rename
   coverage now renames a referenced parent table through a temporary name and
   renames the child table that owns an unnamed foreign key in one
   `RENAME TABLE` statement, verifies an already-open peer observes the final
   parent name, child name, generated `<child>_ibfk_1` constraint identity, and
   referenced parent name, verifies no temporary parent table remains, checks
   `.frm` and `.ibd` movement within the schema directory, inserts a valid
   child row through the moved child, rejects a missing-parent child insert with
   errno 1452, rejects deleting a still-referenced moved parent with errno
   1451, and checks ownerless/native reopen before and after forced `.shm`
   rebuild. Cross-schema foreign-key multi-pair rename coverage now moves both
   the referenced parent table and the child table owning an unnamed foreign key
   from `app` into another schema in one `RENAME TABLE` statement, verifies an
   already-open peer observes target-schema `CONSTRAINT_SCHEMA`,
   `UNIQUE_CONSTRAINT_SCHEMA`, target child/parent table names, and the moved
   generated `<child>_ibfk_1` constraint identity, checks `.frm` and `.ibd`
   movement between schema directories, inserts a valid child row through the
   moved child, rejects a missing-parent child insert with errno 1452, rejects
   deleting a still-referenced moved parent with errno 1451, and checks
   ownerless/native reopen before and after forced `.shm` rebuild.
   Generated-column foreign keys, cyclic/deep cascade chains, and crash
   injection inside referential-action execution remain planned.
   CHECK constraint ALTER coverage adds two named table-level CHECK
   constraints from another ownerless process, verifies an already-open peer
   observes them through `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`, rejects
   invalid rows with errno 4025 while they exist, drops both constraints, and
   verifies the formerly invalid row shape can be inserted plus final
   absent-CHECK checks before and after forced `.shm` rebuild.
   Charset-conversion coverage adds ownerless
   `ALTER TABLE ... CONVERT TO CHARACTER SET utf8mb4`, verifies an already-open
   peer observes `latin1` column metadata before conversion and `utf8mb4`
   metadata after conversion, inserts through the converted table, and verifies
   final converted metadata and rows before and after forced `.shm` rebuild.
   Row-format coverage adds ownerless
   `ALTER TABLE ... ROW_FORMAT=DYNAMIC` over a table created as
   `ROW_FORMAT=COMPACT`, verifies an already-open peer observes the native
   `INNODB_SYS_TABLES.ROW_FORMAT` transition, inserts through the rebuilt
   table, and verifies final metadata and rows before and after forced `.shm`
   rebuild.
   Table-comment coverage adds ownerless
   `ALTER TABLE ... COMMENT='ownerless updated comment'`, verifies an
   already-open peer observes `information_schema.TABLES.TABLE_COMMENT`
   before and after the ALTER, inserts through the table after the metadata
   boundary, and verifies final comment metadata and rows before and after
   forced `.shm` rebuild.
   Force-rebuild coverage adds ownerless `ALTER TABLE ... FORCE`, verifies an
   already-open peer can continue reading through a secondary index after the
   rebuild boundary, inserts through the rebuilt table, and verifies final
   native table metadata, secondary-index metadata, and rows before and after
   forced `.shm` rebuild.
   Column-default coverage adds ownerless `ALTER COLUMN ... SET DEFAULT` and
   `ALTER COLUMN ... DROP DEFAULT`, verifies an already-open peer uses the
   original defaults, then the changed defaults, then fails when omitting a
   NOT NULL column after its default is dropped, and verifies final metadata and
   rows before and after forced `.shm` rebuild.
   Special-index policy coverage
   rejects ownerless `FULLTEXT` and `SPATIAL` index DDL through top-level
   `CREATE INDEX`, `ALTER TABLE ... ADD INDEX`, and inline `CREATE TABLE`
   definitions before MariaDB creates special index metadata or native storage.
   Partition policy coverage rejects ownerless `CREATE TABLE ... PARTITION BY`,
   `CREATE TABLE ... SUBPARTITION BY`, `ALTER TABLE ... PARTITION BY`,
   `ALTER TABLE ... ADD PARTITION`, `ALTER TABLE ... TRUNCATE PARTITION`, and
   `ALTER TABLE ... REMOVE PARTITIONING` before MariaDB creates partition
   metadata or native partition files.
   Tablespace-management policy coverage rejects ownerless
   `ALTER TABLE ... DISCARD TABLESPACE` and
   `ALTER TABLE ... IMPORT TABLESPACE` before MariaDB detaches or imports
   native InnoDB tablespace files.
   Table-admin policy coverage rejects ownerless `ANALYZE TABLE`,
   `CHECK TABLE`, `CHECKSUM TABLE`, `OPTIMIZE TABLE`, and `REPAIR TABLE`
   before MariaDB enters SQL admin handlers that can scan table pages outside
   the proven ownerless `SELECT` snapshot-read surface, update statistics,
   check upgrade state, repair files, or run admin-triggered recreate/rebuild
   paths.
   LOCK TABLES policy coverage rejects ownerless `LOCK TABLES`, `LOCK TABLE`,
   and `UNLOCK TABLES` before MariaDB enters connection-level locked-table mode
   that can keep handler locks alive across later statements.
   FLUSH TABLES lock/export policy coverage keeps ordinary ownerless
   `FLUSH TABLES` working while rejecting
   `FLUSH TABLES ... WITH READ LOCK` and `FLUSH TABLES ... FOR EXPORT` before
   MariaDB enters global read-lock, locked-table, quiesce, or export/checkpoint
   paths.
   Unsafe-hook coverage also kills a process before DDL execution, before
   ownerless DDL finish publishes a stable dictionary generation, and after
   stable dictionary publication. The opt-in stress preset adds broader
   concurrent DDL/DML evidence; external-oracle randomized DDL stress remains
   planned.

Exit criteria:

- DDL and DML remain correct under cross-process load.

### Phase 11: Engine Policy Expansion

Tasks:

1. Decide whether MyISAM, Aria, and MEMORY are allowed in ownerless mode.
   The current policy is InnoDB-only for ownerless read/write opens.
2. If allowed, design per-engine coordination.
3. Otherwise reject them clearly in ownerless mode while keeping them in
   exclusive mode. `MYLITE_OPEN_OWNERLESS_RW` now rejects explicit MyISAM,
   Aria, MEMORY, BLACKHOLE, and non-InnoDB storage-engine default or override
   requests before MariaDB executes the statement.

Exit criteria:

- Engine behavior is explicit, not accidental.

### Phase 12: Application And Stress Validation

Tasks:

1. Run WordPress PHPUnit with multi-process workers over one `.mylite`.
2. Add custom PHP-FPM style concurrent request tests.
3. Add SQLancer/RQG-style random concurrent transaction tests.
4. Add deterministic fault injection for every critical section.
5. Add long-running stress with checksums and MariaDB comparison oracles.
6. Extend the current bounded multi-object reader/writer stress into
   long-running stress with checksums and external oracles. The deterministic
   ownerless SQL stress loop keeps the default CI-sized iteration count in the
   normal embedded preset. The opt-in `ownerless-stress` preset runs the
   independent-table stress case with `MYLITE_OWNERLESS_STRESS_ITERATIONS=200`
   and `MYLITE_OWNERLESS_STRESS_READER_POLLS=400`, plus concurrent DDL/DML
   stress with `MYLITE_OWNERLESS_DDL_STRESS_ROUNDS=8` and same-name temporary
   table stress with `MYLITE_OWNERLESS_TEMP_STRESS_ROUNDS=40`, both with
   forced `.shm` rebuild and native exclusive reopen checks. It also runs
   shared-table checksum stress with
   `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=160`, mixing direct SQL and
   reusable prepared-statement writers while checking sum, version, and
   weighted-sum aggregates against a deterministic oracle before and after
   forced `.shm` rebuild through ownerless and native exclusive reopen. It also
   runs pseudo-random shared-table transaction stress with
   `MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS=120`, padded worker-owned row
   partitions, savepoint rollback, full transaction rollback, bounded rollback
   and retry for MariaDB lock-wait/deadlock errors, a live aggregate reader, final
   sum/version/weighted-sum oracles, and forced `.shm` rebuild plus native
   exclusive reopen checks. The preset also runs explicit
   multi-statement transaction
   stress with
   `MYLITE_OWNERLESS_TX_STRESS_ROUNDS=80`, covering concurrent independent-table
   transactions, savepoint rollback inside every transaction, final aggregate
   oracles, and forced `.shm` rebuild plus native exclusive reopen after the
   workers finish. It also runs active-reader pressure stress with
   `MYLITE_OWNERLESS_ACTIVE_READER_PRESSURE_ROUNDS=48`, holding a
   repeatable-read snapshot pin across repeated writer opens before forced
   `.shm` rebuild and native exclusive reopen checks. Expanding-page pressure
   stress runs the same active-reader shape over distinct large rows with
   `MYLITE_OWNERLESS_EXPANDING_PAGE_PRESSURE_ROWS=48`. Each test has a
   900-second timeout while broader external MariaDB/RQG oracle stress is
   developed.

Exit criteria:

- Ownerless mode has evidence beyond unit tests.

## Test Strategy

Minimum suites before support can be claimed:

- same-process concurrency:
  - multi-handle reads/writes,
  - row lock waits,
  - deadlocks,
  - metadata lock waits.
- cross-process open lifecycle:
  - many concurrent openers,
  - opener crash,
  - `.shm` creation, validation, rebuild, resize, and remap,
  - incompatible `.shm` format rejection,
  - stale copied `.shm` rebuild after closed-directory copy,
  - dirty/rebuilding generation recovery,
  - shared-memory rebuild,
  - process-slot reuse with PID reuse simulation where practical,
  - recovery lock handoff,
  - missed wakeup timeout/rescan.
- shared-memory primitives:
  - cross-process MAP_SHARED visibility,
  - byte-range lock conflict and release-on-death,
  - classic `fcntl` lock close behavior and Linux OFD lock behavior where
    available,
  - Linux futex wait/wake on mapped latch words,
  - fallback wait backend timeout behavior,
  - owner death while holding a shared-memory latch,
  - resize interrupted by process death,
  - SIGBUS prevention by never shrinking active mappings.
- transaction correctness:
  - isolation-level matrix; ownerless `READ UNCOMMITTED` and isolation system
    variable assignments are rejected until cross-process dirty-read semantics
    are designed and the variable-assignment forms can be tracked safely,
  - write skew candidates; SQL coverage now includes a bounded two-row
    serializable write-skew candidate where shared read locks prevent both
    disjoint updates from committing,
  - gap locks,
  - foreign keys, including ownerless peer-visible `ON UPDATE CASCADE`,
    `ON DELETE CASCADE`, `ON DELETE SET NULL`, `ON DELETE RESTRICT`, and
    composite foreign-key coverage plus same-schema and cross-schema
    parent/child rename refresh plus same-schema and cross-schema multi-pair
    parent/child rename refresh,
  - rollback and savepoints.
- page visibility:
  - committed data visible in another process,
  - uncommitted data invisible,
  - long reader with writer and checkpoint; primitive coverage proves a
    checkpoint writer waits behind an active cross-process page-log reader,
  - live idle peer with checkpoint reclamation; SQL coverage proves close-time
    reclamation can checkpoint the page-version WAL while a peer is open with
    no active page-version pin,
  - live snapshot pin with checkpoint reclamation; SQL coverage proves a
    repeatable-read snapshot blocks live-peer prefix compaction until release,
    and killed pinned-reader coverage proves dead-owner cleanup releases the
    reader's MDL, read-view, and pin state so a later live-peer close can
    reclaim; unsafe-hook coverage proves active-pin reclaim can compact
    independent old records when a retained data-page boundary covers the oldest
    live snapshot and, once exactly one active pin remains, product reclaim no
    longer retains checkpointed post-snapshot records; primitive coverage keeps
    required boundary retention and multi-pin newer-record retention separate
    from single-snapshot post-snapshot compaction,
  - consistent-snapshot start pin with deterministic pause; unsafe-hook SQL
    coverage proves the shared pin is published before SQL execution and blocks
    concurrent live-peer close-time reclamation,
  - checkpoint starvation and recovery.
- crash/fault injection:
  - kill writer before/after transaction registration,
    transaction-registration-after-begin SQL hook coverage proves live-peer
    cleanup stays busy and no-live rebuild drops the interrupted update,
  - before/after lock grant,
    before-grant external record-wait SQL hook coverage proves live-peer
    cleanup stays busy and no-live rebuild drops the interrupted waiting update,
    after-grant record-lock SQL hook coverage proves live-peer cleanup stays
    busy and no-live rebuild drops the interrupted update, and primitive
    table-lock waiter-death coverage proves owner cleanup removes a dead
    waiter's shared table-wait entry. SQL-level table-lock fault injection
    remains planned for native table-wait paths, while ownerless SQL
    `LOCK TABLES`/`UNLOCK TABLES` is rejected until SQL locked-table mode has a
    design,
  - before/after page-version append,
    before-append page-version SQL hook coverage proves recovery remains
    readable and later ownerless writes can proceed after a writer is killed
    before appending a page-version WAL record,
  - after redo bytes are marked written but before latest-checkpoint publish,
  - after volatile page-visible publish but before durable checkpoint,
  - before/after commit publish,
  - during checkpoint,
  - during DDL.
- application:
  - WordPress PHPUnit in multi-worker mode,
  - wpdb mysqli API compatibility,
  - PDO transaction tests.
- filesystem/platform:
  - APFS local,
  - ext4 local in Linux CI,
  - tmpfs,
  - explicit rejection for unsupported or unproven filesystems.

## Compatibility Impact

Ownerless concurrency is not only a `libmylite` lifecycle feature. It affects:

- SQL transaction semantics,
- isolation levels,
- lock wait timeout,
- deadlock diagnostics,
- metadata lock behavior,
- DDL visibility,
- engine availability,
- crash recovery,
- read-only opens,
- directory copy/backup rules,
- filesystem support policy.

Directory copy and backup status must stay explicit: copying a closed directory
is allowed only if the next opener can discard/rebuild `.shm`; copying an open
directory is unsupported until an ownerless backup protocol coordinates reader
slots, checkpoints, and page-version retention.

Compatibility status should stay partial until at least Phase 9 passes. Shared
read-only opens can be claimed for the tested SQL policy and committed-read
visibility surface, including prepared `SELECT` execution, read-only
transaction first-read/repeatable-snapshot behavior, reads inside transactions
after local writes, and no-live-process page-version replay; true
InnoDB `innodb_read_only` startup, ownerless cross-process dirty reads, and full
DDL/file-lifecycle tablespace recovery replay remain planned. Current product
no-live replay skips retained page-version records for tablespaces no longer
present, but it still lacks durable file lifecycle metadata for broader DDL
recovery.

## Binary Size Impact

The final ownerless mode will add code and shared-memory/log machinery. It is
opposed to the size-reduction track. Size work should not optimize away InnoDB
subsystems that this mode needs:

- transaction system,
- lock system,
- redo/recovery,
- purge,
- dictionary,
- MDL,
- DDL recovery,
- page IO and checkpoint code.

## Risks

- This is a major fork of InnoDB behavior.
- Process-shared synchronization is platform-sensitive and hard to test.
- InnoDB uses raw pointers in many concurrency structures; shared state must
  use stable IDs and local pointer caches.
- Page visibility may require a MyLite page-version log, effectively adding a
  second physical logging layer.
- Long readers can starve checkpoint progress, as in SQLite WAL.
- The feature may force ownerless mode to be InnoDB-only for a long time.
- Bugs are likely to be corruption bugs, not simple query failures.
- Network filesystems should remain unsupported unless a later design proves
  correct locking and shared-memory behavior.
- External shared-memory objects would add lifecycle and identity risks. They
  should remain optional future backends, not the first implementation.
- Opaque platform synchronization objects in `.shm` can create ABI and
  recovery traps. The stable format should use MyLite-owned fixed-width latch
  words and backend-specific wait code.
- Classic POSIX `fcntl` locks can be released by closing an unrelated file
  descriptor for the same file. MyLite must centralize lock-file descriptors,
  prefer OFD locks on Linux when practical, and test this failure mode.
- Intermediate MDL, transaction, and lock-manager phases can look functional
  before page visibility and redo are safe. Product ownerless read/write opens
  must stay disabled until the full commit/recovery path passes fault tests.

## Acceptance Criteria For The Full Feature

- No owner process, daemon, broker, or hidden server exists.
- No directory-wide exclusive read/write lock is held during ordinary work.
- At least four independent processes can open the same `.mylite` directory and
  execute mixed read/write InnoDB transactions.
- Non-conflicting writers make progress concurrently.
- Conflicting writers block, timeout, or deadlock with MariaDB-compatible
  behavior.
- Readers see stable snapshots and never see uncommitted data.
- Cross-process DDL and DML coordinate through metadata locks.
- Process crashes at every critical phase recover without corruption.
- All durable and transient state remains inside the database directory.
- `mylite-concurrency.shm` is file-backed, mapped with shared visibility,
  rebuildable after crash, and never required as the only durable copy of
  committed database state.
- Ownerless mode rejects platforms or filesystems that fail mmap, byte-range
  lock, release-on-death, resize/remap, or wait-backend probes.
- Closed-directory copies rebuild stale `.shm` safely, while open-directory
  copies are rejected or documented unsupported until a backup protocol exists.
- Hot uncontended shared-memory paths avoid kernel calls on the primary Linux
  backend, and contention metrics prove the design is not accidentally
  serializing all operations through a single global latch.
- Unsupported filesystems and unsupported engines fail explicitly.
- Compatibility docs and roadmap describe the precise limits.

## Recommendation

Do not start by attempting full ownerless cross-process writes. Start with:

1. same-process concurrency coverage,
2. shared read-only opens,
3. negative proof and source-backed experiments,
4. shared-memory foundation,
5. cross-process MDL and transaction visibility,
6. only then page visibility and write commits.

This is the only path that keeps the project honest. It also gives useful
deliverables before the full ownerless write design is complete.
