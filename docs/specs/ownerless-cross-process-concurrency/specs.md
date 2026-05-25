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
  a replacement for InnoDB redo until a dedicated page-versioning layer exists;
  it initially records process registry, lock-manager, dictionary-generation,
  and recovery metadata.
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
read-view registry, a fixed InnoDB table/record lock registry, and a
redo-visibility state segment. Current opens publish one active process slot
for the embedded runtime process, assign that slot the wait-channel range, mark
`.shm` dirty while the runtime is active, and release the slot before returning
`.shm` to clean state on final close. Clean opens preserve the existing
segments. A later open treats dirty, rebuilding, invalid, or no-live-process
stale state as volatile coordination state, rebuilds the segments, and
increments the recovery-generation field. Hot registry latches use a
fixed-width 32-byte MyLite latch ABI with an atomic packed state/owner-slot
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
segments are not active in the production `.shm` layout yet. The transaction
registry has latch-protected
monotonic transaction ID allocation, active transaction snapshots sorted for
future read-view construction, oldest-active tracking, stale end rejection, and
owner-scoped active-count checks over file-backed `MAP_SHARED` mappings.
Durable opens map the `.shm` file with `MAP_SHARED` to validate the published
layout before starting MariaDB. InnoDB now has guarded MyLite hook surfaces for
transaction ID allocation, read-write transaction registration, transaction
serialisation-number assignment, active-ID snapshots, maximum transaction ID
reads, deregistration, read-view publication, table/record locks, waits, and
redo visibility. Internal or recovered InnoDB transactions that were never
registered in the ownerless shared transaction registry still receive
serialisation numbers from the shared monotonic sequence, and missing
deregistration is treated as a no-op; registered read-write transactions
continue through the shared registry.

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

1. Add shared directory locks for read-only open.
2. Start InnoDB in true read-only mode for readers.
3. Use separate per-process `run/` and `tmp/` subdirectories.
4. Fail read-only open if recovery is required and no writer/recovery opener is
   available.
5. Allow multiple read-only processes when no writer is active.

Exit criteria:

- Multiple read-only processes can query a clean database.
- Read-only opens never write durable state.

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
   and `.ckpt` files exist with UUID-bound headers, but durable coordination
   records and recovery replay remain pending. `MYLITE_CAP_OWNERLESS_RW`
   remains explicitly gated off until SQL lock, transaction, page-visibility,
   and recovery paths use ownerless coordination.
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
   probe; a product open-mode decision still must combine those results with
   SQL lock, transaction, page-visibility, and recovery readiness before
   enabling any ownerless SQL mode.
9. Add crash tests for opener death, stale shared memory, process-slot reuse,
   resize interruption, recovery lock handoff, and waiters surviving missed
   wakeups.
10. Keep ownerless read/write open unavailable outside tests.

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
   and an internal cross-process shared/exclusive lock-table primitive with
   compatible shared holders, blocking exclusive conflicts, repeated-owner
   reference counts, same-owner mode upgrades, release wakeup, and timeout
   coverage. It also has stable ownerless schema/table key hashing shaped after
   MariaDB's namespace/database/name MDL key structure. Product opens route
   MariaDB schema/table MDL through this segment using the runtime process-slot
   owner while the exclusive directory lock is still held. It does not model
   intention or deadlock semantics yet.
2. Replace or wrap process-global `MDL_map` operations for MyLite ownerless
   mode. The current MariaDB patch adds a MyLite hook surface on the embedded
   MDL ticket lifecycle and covers balanced acquire/release events for
   schema/table tickets, including cloned tickets, upgrades, and downgrades.
   `libmylite` registers that hook against the directory-backed MDL lock-table
   segment with the current runtime process-slot owner. The exclusive directory
   lock still prevents cross-process SQL metadata-lock enforcement in product
   opens.
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
   redo/checkpoint, and recovery phases are complete.
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
  stable committed data.
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
   opens register that bridge against the directory-backed InnoDB lock-registry
   segment while the exclusive directory lock remains in place. The registry
   now stores directory-owned wait edges, local InnoDB waits publish and clear
   those edges under embedded SQL coverage, and the primitive detects
   cross-process wait cycles. InnoDB table and record grant paths now perform
   a nonblocking shared-registry reservation before granting a native lock. On
   external conflicts, InnoDB now creates a local waiting lock, publishes a
   directory-owned wait edge, sleeps on the mapped wait word without holding
   local InnoDB latches, refreshes redo visibility after wake, and retries the
   native grant. Ownerless write commits now flush dirty pages through the
   transaction commit LSN before releasing shared lock-registry entries, which
   avoids the previous whole-buffer-pool sync while still keeping the current
   test-gated visibility bridge conservative. The guarded
   `ownerless-test-hooks` preset now exercises this path through
   `MYLITE_OPEN_OWNERLESS_RW` instead of the raw directory-lock bypass
   environment variable.
3. Add cross-process wait/wakeup/deadlock detection.
   The lock registry stores wait edges by stable owner and transaction IDs,
   wakes waiters when active slots are released, and detects wait cycles before
   the SQL lock-wait timeout.
4. Add timeout and victim-selection tests.
   Guarded SQL tests now cover non-conflicting writers, same-row writer waits,
   reverse-order row deadlocks, stale committed reads after an external write,
   and cleanup of wait state after timeout/deadlock.

Exit criteria:

- Conflicting cross-process writers block and deadlock like InnoDB writers in
  one process.
- Ownerless read/write opens remain disabled; conflict behavior is verified
  through guarded integration tests until page visibility, redo, and commit are
  safe.

### Phase 8: Page Visibility Prototype

Tasks:

1. Build an experimental page-version log below InnoDB page IO.
   The current first-party primitive defines a fixed-header page-version log
   record format, serializes appends with a directory-file byte-range lock,
   supports `space_id=0` and `page_no=0` for real InnoDB identifiers, reads
   the newest version visible at or below a caller-supplied commit LSN, and
   tolerates an incomplete tail record left by an interrupted append. Primitive
   tests cover same-process visibility, too-small read buffers, missing pages,
   and cross-process append serialization. It is not wired into production
   `.wal` creation, InnoDB page IO, checkpointing, or recovery yet.
2. Teach page reads to consult page-version state before tablespace files.
3. Publish commit end marks and reader snapshots.
4. Implement passive checkpoint of safe page versions into tablespace files.
5. Run kill tests around write, commit publish, checkpoint, and recovery.

Exit criteria:

- A process can read committed pages written by another process without waiting
  for all pages to be flushed directly to tablespace files.

### Phase 9: Cross-Process Redo And Commit

Tasks:

1. Globalize LSN allocation.
2. Serialize or atomically reserve redo append ranges across processes.
3. Define group commit or safe serialized commit.
4. Reconcile InnoDB redo with MyLite page-version visibility.
5. Add power-fail style crash tests with fault injection.

Exit criteria:

- Committed cross-process writes survive process crashes and full restart.

### Phase 10: DDL, Dictionary, And Space Allocation

Tasks:

1. Coordinate table ID and space ID allocation across processes.
2. Coordinate create, drop, truncate, rename, and online DDL.
3. Add dictionary generation invalidation in every process.
4. Add broad DDL compatibility tests.

Exit criteria:

- DDL and DML remain correct under cross-process load.

### Phase 11: Engine Policy Expansion

Tasks:

1. Decide whether MyISAM, Aria, and MEMORY are allowed in ownerless mode.
2. If allowed, design per-engine coordination.
3. Otherwise reject them clearly in ownerless mode while keeping them in
   exclusive mode.

Exit criteria:

- Engine behavior is explicit, not accidental.

### Phase 12: Application And Stress Validation

Tasks:

1. Run WordPress PHPUnit with multi-process workers over one `.mylite`.
2. Add custom PHP-FPM style concurrent request tests.
3. Add SQLancer/RQG-style random concurrent transaction tests.
4. Add deterministic fault injection for every critical section.
5. Add long-running stress with checksums and MariaDB comparison oracles.

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
  - isolation-level matrix,
  - write skew candidates,
  - gap locks,
  - foreign keys,
  - rollback and savepoints.
- page visibility:
  - committed data visible in another process,
  - uncommitted data invisible,
  - long reader with writer and checkpoint,
  - checkpoint starvation and recovery.
- crash/fault injection:
  - kill writer before/after transaction registration,
  - before/after lock grant,
  - before/after page-version append,
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

Compatibility status should stay "planned" until at least Phase 9 passes. A
partial status after Phase 3 can claim shared read-only opens only.

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
