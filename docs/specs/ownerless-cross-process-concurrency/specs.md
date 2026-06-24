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

Ownerless concurrency needs additional directory-owned coordination state.
Fresh ordinary exclusive read/write opens do not create this subtree; it is
created by ownerless/shared-readonly opens, or reused by ordinary opens only
when durable ownerless runtime files already exist and must be validated or
recovered:

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
    mylite-runtime-startup.lock
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
- `mylite-runtime-startup.lock`: separate byte-range lock anchor for ownerless
  native runtime startup. It is separate from `mylite-concurrency.lock` so
  nested bootstrap locks on the main coordination file cannot release the
  startup boundary through classic `fcntl()` close semantics.
- `mylite-concurrency.shm`: memory-mapped shared state. It is transient and can
  be rebuilt from durable logs, but while active it is the common coordination
  memory for all processes.
- `mylite-concurrency.wal`: MyLite multi-process coordination log. This is not
  a replacement for InnoDB redo. Its fixed recovery header is followed by the
  ownerless page-version payload. Guarded ownerless SQL now appends dirty page
  images before the temporary commit-LSN flush bridge releases shared locks.
  The shared page-version index can rebuild and checkpoint those records. The
  page-log checksum remains a full reconstructed page checksum independent of
  the selected payload encoding; ownerless InnoDB publish hooks may precompute
  that checksum after accepting a page for append and hand it to the page-log
  encoder to keep the append path from rescanning the page.
  Guarded ownerless SQL can use page-version reads for direct or prepared
  `SELECT`/`WITH` statements at a live page-version read LSN, while the
  page-visible LSN remains the durable recovery/checkpoint boundary. Eligible
  handles keep that read LSN monotonic and publish a shared page-version pin
  before clean-page refresh. When no page-visible LSN exists yet, the baseline
  read pin is still enough to classify eligible statements as ownerless plain
  reads, but it does not expose an external page boundary. Successful direct
  `mylite_exec()` reads keep the
  handle pin after returning until a replacement read, non-read/current-read
  statement, error, close, or dead-owner cleanup releases it, while prepared
  result cursors keep the handle pin until the result is exhausted, reset, or
  finalized; active result pins block live-peer and single-owner checkpointing
  so WAL cannot be discarded while the handle may still have stale clean pages.
  Eligible retained reads reject lower visible-boundary overlays for user
  data/index/blob pages during visible-boundary external refresh, clean-page
  refresh, and file-read overlay, while current live reads still accept current
  ownerless page images that advance the page. Native undo, system,
  allocation, and recovery pages still use ordinary visible-boundary refresh.
  The rollback-segment and undo-header history-proof pages remain part of the
  current ownerless write proof; unsafe-hook coverage now forces native-support
  page-version publication failure and verifies the native history flush
  fallback before any smaller replacement proof can be claimed. The
  rollback-segment proof page may use a hinted history-rseg page-log delta
  payload after a standalone base record exists, but that optimization does not
  elide the proof page and does not enable broad SYS-page delta encoding.
  The native-support proof-only WAL shape keeps the rollback-segment and
  undo-header proof records durable but stores them as page-log metadata with
  zero payload bytes. Page-image reads, latest scans, replay, and checkpoint
  retained-record callbacks skip those records so a `.shm` page-version index
  rebuild never points at unreadable proof metadata. No-live native reclaim
  uses a dedicated proof-including scan for those records, forces the owning
  native tablespace to flush through the proof page LSN, and keeps the DML
  marker/WAL until disk page-LSN proof shows the rollback-segment and undo
  history obligations are native-durable.
  Same-runtime reads covered by the handle's local-native autocommit write
  boundary do not publish a page-version pin or enable the file-read overlay
  while the runtime remains in a continuous single-owner epoch and has not
  started with or consumed retained page-version WAL. That keeps ordinary local
  verification reads on native InnoDB pages and reserves page-version read state
  for handles that actually consume ownerless page WAL. Autocommit writes
  outside that proof still seed the handle's page-version read LSN, preserving
  multi-process read-your-writes and retained-overlay behavior. A
  new ownerless process generation or an advancing handle pin forces clean-page
  refresh without the single-owner skip, because peer commits may already be
  native-checkpointed and reclaimed from the page-version WAL. Live raw-latest
  promotion is used
  only when no other active native transaction or active redo reservation can
  prove a lower or uncommitted page image still matters and the older durable
  boundary is retained by external page-version pins; older repeatable-read
  snapshot pins retain WAL for those readers without forcing unrelated
  autocommit plain reads down to the durable page-visible boundary.
  Repeatable read and serializable transactions pin that live read LSN on their
  first consistent read. `START TRANSACTION WITH CONSISTENT SNAPSHOT` reads the
  shared ownerless redo state and publishes its pin before SQL execution; when
  no ownerless write transaction or redo reservation is active, that pin can use
  the live ownerless read LSN so later repeatable-read transactions see commits
  that completed before they started even while older pins retain WAL. When no
  ownerless page-visible LSN has been published and the page-version WAL has no
  payload records, the ownerless writer can seed that pin from the current
  native InnoDB checkpoint LSN at snapshot start instead of relying on ownerless
  hooks during ordinary startup.
  The shared page-version index is an acceleration cache over the append stream:
  readers that hit the index directly must validate the WAL tail after the
  indexed record before returning, because a peer append can become visible to
  the page-log reader before the peer publishes the matching page-index entry.
  Readers also track a visible-generation counter so a newly published physical
  page boundary at the same visible LSN still refreshes clean buffered pages.
  Equal InnoDB page LSNs are not an ordering proof for retained ownerless page
  boundaries: live refresh overlays only forced or strictly newer page-version
  images, retained current live reads reject lower/equal boundary images only
  after the same handle has already observed the same user page at an
  equal-or-newer ownerless commit boundary or when the page-log read selected a
  record flagged as a synthesized snapshot boundary, and product no-live
  tablespace replay keeps an existing matching native disk page when its page
  LSN equals the retained WAL image.
  Before no-live reclaim discards retained page-version WAL, the runtime
  publishes eligible native support/allocation/system buffer-pool pages to the
  reclaim LSN, waits for native dirty pages to flush, and takes the native
  checkpoint. User data/index page images require transaction-owned
  page-version records or native snapshot-boundary synthesis before their
  ownerless page boundary records are compacted. Native snapshot-boundary
  synthesis is suppressed during dictionary DDL and the post-DDL
  conservative-write window because page LSN does not prove that a newly
  created file-per-table tablespace existed at an older reader snapshot; the
  same window suppresses external space-allocation refresh so local post-create
  allocation pages are not refreshed from retained external state.
  Non-DDL no-live DML reclaim also scans checkpointable user tablespace
  page-version records before truncation and retains the WAL unless the native
  tablespace page on disk has a `FIL_PAGE_LSN` newer than the record page LSN,
  or the same `FIL_PAGE_LSN` plus a byte-for-byte match with the retained
  payload. When a DML-specific native checkpoint marker is pending with retained
  page-version records and no dictionary file-op or autoincrement marker, MyLite
  drains the marker for autocommit and single-owner explicit writers whose local
  close can prove the checkpoint boundary. Peer-explicit DML evidence is still
  retained while a peer is live, but final no-live close now forces native
  checkpoint coverage and reuses the per-page native image/absence/discard
  proof before clearing the DML-only marker and checkpointing the page-version
  WAL. Generic dictionary/file-op and autoincrement markers keep their stricter
  policy. Live-peer reclaim keeps checkpointable user data/index page records
  in WAL until no-live reclaim can make the native data file authoritative.
  Focused Linux coverage now also commits post-checkpoint file-per-table DML in
  a child process that exits with `_exit(0)` before `mylite_close()`, verifies
  the durable DML marker/WAL while that child is still an unreaped zombie, and
  proves the next ownerless opener preserves the committed row before no-live
  native proof drains the marker/WAL.
  A paired killed-uncommitted explicit transaction case forces the same
  post-checkpoint file-per-table DML path before `COMMIT`, kills the writer,
  requires both native file-operation markers to remain clear, and proves
  no-live ownerless recovery plus forced `.shm` rebuild keep the
  pre-transaction row authoritative.
  When no-live reclaim advances the durable
  checkpoint-visible LSN, the still-existing volatile redo state is reseeded
  from that checkpoint so readers do not observe `.shm` metadata behind `.ckpt`
  before a later `.shm` rebuild. Focused no-live cutover coverage now binds a
  reclaimed bulk-insert visible LSN to native InnoDB checkpoint coverage,
  removes both `.wal` and `.shm`, and verifies ordinary native reopen without
  page-version WAL overlay.
  Transactions that already performed local writes or locking reads avoid
  global refresh, and clean-page refresh skips locally dirty buffer pages.
  DML/DDL, recovery, checkpointing, and tablespace replay still use the
  conservative native-file bridge until broader recovery is implemented.
- `mylite-concurrency.ckpt`: durable checkpoint/progress metadata for rebuilding
  shared coordination state. Hook-driven raw-latest and page-visible checkpoint
  writes use the already-published shared redo state as the monotonic source
  while holding the checkpoint byte-range lock, so they avoid rereading the
  `.ckpt` payload without moving latest or visible LSNs backward. Startup
  baseline seeding and no-live native checkpoint promotion still merge against
  the `.ckpt` payload directly because their proof can come from native
  checkpoint state rather than a just-published redo-state pair. The
  latest/visible pair now has two appended checksummed generation records after
  the legacy payload; readers prefer the highest valid generation, fall back to
  the legacy pair only while both record slots are empty, and fail closed when
  non-empty record slots contain no valid checksum. Repeated same-pair
  checkpoint publications skip new record generations for non-durable updates;
  durable same-pair updates skip only when a process-local sync anchor proves
  that the same process already synced the same pair on the same checkpoint fd.
  Advancing checkpoint updates also skip rewriting the legacy latest/visible
  payload once a valid checksummed generation record exists; empty-record
  fallback still initializes the legacy pair, and non-empty invalid record
  slots still fail closed rather than falling back to stale legacy bytes.
  Redo-state-backed hook writes also maintain a process-local latest/visible
  generation-record cache while the process registry proves this runtime is the
  only active owner and no peer owner has joined since registration; peer
  presence, direct native checkpoint writes, startup seeding, and no-live native
  checkpoint promotion reset the cache and keep the file-read merge path.
  Cross-process group commit, broader checkpoint batching, and the separate
  native file-operation marker remain bounded by their existing proof rules.
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
The `ownerless-closed-copy-shm-rebuild` slice now stores the `.shm` file's
current device/inode identity in the stable header and rebuilds volatile
segments when a copied header's database UUID matches but the shared-memory
file identity no longer matches the file being opened.

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
prepared `SELECT`/`WITH` statements with table references at a live
page-version read LSN while the page-visible LSN remains the durable
recovery/checkpoint boundary. Non-locking tableless `SELECT`/`WITH`
statements with no `FROM` or `JOIN` token skip page-version reads and global
page refresh because MariaDB has no storage table to open for them. Eligible
handles keep the page-version read LSN monotonic and open a shared read pin
before clean-page refresh. The zero-boundary path also opens the baseline read
pin and enables the plain-read scope for eligible table-reading
`SELECT`/`WITH` statements without enabling external page visibility.
Successful direct reads keep that pin until a replacement read,
non-read/current-read statement, error, or close;
autocommit live raw-latest promotion is disabled while another active native
transaction or an active redo reservation is present. A separate repeatable-read
snapshot pin only retains WAL for that reader, and can permit unrelated
autocommit plain reads to use the newer page-version boundary once native
transaction state is idle. Autocommit writes also advance a local-native read
boundary that is not a page-version read. During a continuous single-owner
epoch that did not start with retained page-version WAL and has not consumed
page-version WAL, eligible same-runtime reads covered by that boundary skip
shared page-version pins and the InnoDB file-read overlay. Outside that proof,
the same autocommit write also advances the real page-version read LSN so
peer-era reads keep the existing page-version pin and retained-refresh path.
Pressure-limit `MYLITE_BUSY` write rejections release the handle's transient
autocommit read pin before returning, so blocked writers do not keep retained
page-version WAL alive after the reader that caused the pressure exits.
Direct autocommit result reads on pressure-limited handles also release their
transient pin after producing the result.
Eligible autocommit page-version reads close the
current InnoDB read view at statement start, so later statements can observe
new peer commits. Repeatable read and serializable transactions pin the live
read LSN on their first consistent read, while transactions with local writes
or locking reads avoid global refresh and clean-page refresh skips locally
dirty buffer pages. Ownerless page-write hooks avoid page-write ownership for
SQL `SELECT`, including locking reads such as `SELECT ... FOR UPDATE`, leaving
row-lock and current-read waits on the native InnoDB paths. The
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
latest shared page visibility. Generation refresh now runs before the
statement's page-version read decision, releases any handle page-version pin,
closes the current InnoDB read view, clears external page observations, resets
handle page-version/native read watermarks, refreshes external space headers
from page 0, and evicts clean external pages. Already-open peers that observe a
generation change stay in a conservative native-read mode instead of
immediately re-enabling page-version table reads, because the current
page-version WAL key is only `(space_id, page_no)` and does not encode a table
copy/rebuild generation. In that peer-observed conservative mode, autocommit
statements that refresh current data use a forced native visible-boundary
buffer-pool refresh that skips page-version WAL and page-version negative-cache
proofs. That keeps rebuilt/compressed table reads native-only while allowing
the same handle to observe peer commits, cascades, and deletes even if it
previously cached the affected pages locally.
If an ownerless autocommit plain read still reaches MariaDB errno `1932`
against a peer-created file-per-table InnoDB table, direct and prepared
`SELECT`/`WITH` statements refresh native pages, flush SQL table caches, evict
unused InnoDB dictionary-cache entries, clear MyLite's ownerless FK cache, and
retry once. Prepared retry uses the saved SQL text and parameter bindings to
prepare a replacement native statement before the second execute. Mutating
statements, DDL, locking reads, and explicit-transaction statements do not use
this retry path.

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
shared-memory file device/inode pair observed by the opener, and a
concurrency-generation field in `mylite-concurrency.meta`. The UUID survives
closed-directory copies; process slots do not. If the copied `.shm` header's
file identity differs but the UUID matches, the opener must assume the volatile
state came from another directory instance and rebuild `.shm` before use.

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
  through group commit. Current page-visible publication still syncs changed
  page-version WAL before publishing visibility, but may skip the clean sync
  when the current WAL size and page-log header generation exactly match a
  process-local already-synced anchor. Production performance probe output now
  also summarizes explicit-transaction ownerless insert attribution so per-row
  undo/MTR/page-log costs and final commit visibility costs are visible
  separately from autocommit summaries. The same production probe also splits
  ordinary embedded release shutdown into `mysql_thread_end()` and
  `mysql_server_end()` timing while retaining the existing aggregate shutdown
  counter, so process-isolated PHPUnit lifecycle cost can be attributed before
  any MariaDB cleanup trimming work. Follow-up cleanup attribution splits
  `mysql_server_end()` through `end_embedded_server()` and `clean_up()` and
  currently points the dominant ordinary shutdown cost at MariaDB
  `plugin_shutdown()`, not ownerless SHM/WAL/checkpoint setup. Plugin-shutdown
  attribution then narrows that cost further to storage-engine plugin
  deinitialization in the `reap_plugins()` path: a reduced production sample
  reported `223.800 ms` across 9 storage-engine deinit calls while
  information-schema plugin deinit remained `0.008 ms`. Storage-engine
  finalization attribution then identifies InnoDB handlerton panic shutdown as
  the owner of that cost: a reduced production sample reported `216.159 ms`
  across 9 storage-engine finalizers, `216.142 ms` in `hton->panic`, and
  `215.685 ms` in the single InnoDB finalizer. InnoDB shutdown attribution then
  points at `logs_empty_and_mark_files_at_shutdown()` waiting rather than dirty
  flush/checkpoint work: a reduced production sample reported
  `248.695 ms` in `innodb_shutdown()`, `232.035 ms` in log-empty shutdown, and
  `200.309 ms` in the fixed shutdown-loop sleep while checkpoint work was
  `0.011 ms`. Embedded builds now skip the first log-empty sleep and immediately
  run the existing quiet-state checks; a reduced production sample after that
  change reported `125.473 ms` in `innodb_shutdown()`, `101.397 ms` in
  log-empty shutdown, and `100.470 ms` remaining in the retry sleep while
  checkpoint work remained `0.017 ms`. A follow-up embedded-only bounded
  immediate-retry budget removes the remaining fixed sleep for the ordinary
  warm sample: `innodb_shutdown()` dropped to `16.315 ms`, log-empty shutdown
  dropped to `1.569 ms`, and `innodb_logs_empty_sleep_ms` was `0.000`.
  Repeated open/close attribution then showed the remaining fixed sleeps were
  background-thread waits, not active-transaction or checkpoint retries: a
  ten-iteration reduced production sample reported 9 sleeps after background
  retries and `902.541 ms` total sleep time. Embedded background-thread retry
  sleep now uses a `1 ms` poll after the immediate retry budget is exhausted;
  a ten-iteration sample after that change reported `11.280 ms` total
  log-empty sleep and `54.854 ms` total log-empty shutdown.
  Startup attribution now splits `mysql_server_init()` through embedded server
  startup, server components, plugin initialization, storage-engine
  handlerton initialization, and InnoDB `srv_start()`. A reduced production
  sample after the shutdown fixes reported `94.364 ms` average in
  `mysql_server_init()`, `52.325 ms` in server-component plugin
  initialization, `45.038 ms` in InnoDB storage-engine initialization, and
  `44.983 ms` in InnoDB `srv_start()`, keeping the remaining startup target on
  native MariaDB/InnoDB startup rather than ownerless coordination. Ownerless
  direct/prepared tableless `SELECT 1` probes now skip page-version read setup
  and global page refresh. A fresh reduced production sample before that
  fast-path showed sub-millisecond active runtime reconnects, but ownerless
  direct/prepared `SELECT 1` throughput at only about `0.66x` ordinary. A
  reduced production sample after the fast-path moved the direct ratio to
  `0.7871` and the prepared ratio to `0.9292`, leaving write-path
  page-publication/checkpoint work as the larger remaining performance gap.
  The production probe also carries real InnoDB primary-key point-select timing
  for ordinary and ownerless direct/prepared reads, plus ownerless direct exec,
  prepared-step, and page-version read-hook attribution under the existing
  stats flag, so tableless probe overhead can be separated from real-table read
  overhead before changing refresh policy.
  A follow-up read-hook attribution slice extends the same read windows with
  ownerless MDL, transaction, and read-view callback counts and elapsed time,
  separating native hook cost inside `mysql_query()`/`mysql_stmt_execute()`
  from page-version WAL reads and statement-boundary refresh. It is
  diagnostics-only and does not change ownerless snapshot publication, MDL
  blocking, DDL, active-reader, or peer join/leave policy. A reduced
  100-select production sample reported zero native hooks for tableless reads,
  roughly one MDL acquire/release, one transaction snapshot, and one read-view
  register/deregister per point select, and about `0.008-0.011 ms/select` in
  those hook callback bodies.
  A refresh attribution follow-up then split the ownerless statement-boundary
  refresh stage itself. A reduced 100-select production sample reported
  tableless reads doing no shared snapshot, pin, clean-page refresh, visibility,
  or native flush work, while point selects used the local-native-current-read
  path and spent nearly all refresh time in the shared
  redo/process/transaction snapshot (`0.034-0.035 ms/select`).
  The single-owner refresh-snapshot fast path then used process active
  count/generation to infer the negative peer checks without scanning process
  and transaction registries when the current owner is alone. A reduced
  stats-enabled point-select sample moved shared-snapshot time to
  `0.001 ms/select`, and a 1000-select stats-off sample reported direct
  point-select ratio `0.9028` and prepared point-select ratio `0.7707`.
  A follow-up point-select engine attribution slice added opt-in SQL handler
  `ha_index_read_*()` and InnoDB `index_read()` child-stage counters to the
  production probe. Its first reduced stats-enabled sample reported ordinary
  direct/prepared point-select `row_search_mvcc()` at about
  `0.006 ms/select`, ownerless direct/prepared point-select
  `row_search_mvcc()` at about `0.012 ms/select`, no page-version reads, zero
  native prepared reprepare/close calls, and ownerless prepared-step native
  execute at `0.303 ms/select`; a stats-off 3000-select sample reported
  point-select direct/prepared ratios of `0.8475`/`0.8276`.
  Native redo attribution then showed repeated actual rebuilds came from a
  physical clean-shutdown `ib_logfile0` tail (`100663304` observed versus
  the then-configured `100663296`). Embedded clean-shutdown tail truncation now
  normalizes that tail only after MariaDB's clean shutdown LSN/checkpoint
  checks pass; the focused reduced production probe reported zero warm-open
  redo rebuilds and `124.153 ms` ordinary warm open/close, while the public
  open/close bench left `ib_logfile0` at the configured size after close. The
  embedded redo log size profile later changes that configured size to
  `16777216` bytes to reduce repeated clean redo scan cost while leaving native
  recovery and rebuild predicates in MariaDB; its focused before/after
  production probe moved ordinary `startup_innodb_recovery_start_ms_avg` from
  `45.391 ms` to `23.570 ms` and ownerless from `32.755 ms` to `20.636 ms`,
  with zero warm-open redo rebuilds in both samples.
- Page-version lookup should be O(1) average by `(space_id, page_no)` with a
  short version chain filtered by reader end mark.
- Ordinary exclusive opens must stay on the native MariaDB embedded hot path:
  they may create and validate fixed ownerless coordination files, but they
  must not install ownerless runtime lifecycle, MDL, transaction, read-view, or
  InnoDB hook callbacks, run ownerless statement machinery, or publish
  page-version WAL payloads unless the handle uses `MYLITE_OPEN_OWNERLESS_RW`,
  `MYLITE_OPEN_SHARED_READONLY`, or a retained page-version WAL payload
  requires native exclusive replay.
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

Current implementation runs this probe under the prepared database directory
before the ownerless startup lock and embedded MariaDB runtime startup. A
successful probe writes `concurrency/mylite-ownerless-platform.meta` with the
database-directory device id and `required_primitives=1`; later ownerless opens
reuse that proof and re-probe when the proof is absent or the database directory
is on a different filesystem. The opener also remembers successful proofs per
filesystem device inside the current process; a later fresh MyLite directory on
the same device may skip the child-process probe, but it still writes its own
directory-local proof metadata before ownerless mode is accepted. Unsafe
probe-failure hooks bypass the process cache so negative coverage still
exercises the real probe path. The open gate requires the correctness
primitives; the fast wait backend remains high-performance evidence rather
than a correctness requirement.

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
critical sections. Linux liveness also treats `/proc/<pid>/stat` state `Z` as
dead owner state because a zombie process has exited and cannot release
ownerless resources even though `kill(pid, 0)` still succeeds before parent
reap.

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
   byte-range locks and mmap semantics. Ownerless read/write and shared
   read-only opens now run a database-directory probe before runtime startup and
   persist a device-bound proof to avoid repeating the full probe on later opens
   of the same directory.

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
   probe. Ownerless opens now probe the prepared database directory, persist a
   device-bound successful proof in `concurrency/mylite-ownerless-platform.meta`,
   and reject directories whose backing filesystem cannot prove the required
   primitives. `MYLITE_OPEN_OWNERLESS_RW` now uses the product ownerless startup
   path in normal embedded builds; unsupported surfaces remain tracked
   explicitly in the compatibility matrix.
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
   directory lock remains in place. Snapshot reads through
   `trx_sys_t::get_max_trx_id()` and `trx_sys_t::snapshot_ids()` now retry
   transient ownerless snapshot hook errors before preserving the existing
   persistent-error abort path.
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
   prepending a new undo log, and force a clean-page refresh for that exact
   first history-list page after it is latched, so the file-list splice does
   not read stale links from another process's buffer pool. Physical undo
   tablespace truncation stays disabled until that path has directory-owned
   rollback-segment metadata rebuild coverage.
5. Add crash cleanup for active transactions from dead process slots.
   Product opens now detect dead owners with active transaction/read-view/lock
   state and preserve that state while live peers remain. A guarded
   cross-process SQL test kills an uncommitted ownerless writer, verifies that a
   concurrent opener receives busy while another ownerless peer is live, then
   verifies that a no-live-process reopen rebuilds volatile shared state and
   sees only committed rows. A focused Linux SQL regression also leaves an
   exited committed writer unreaped as a zombie and verifies the dead owner slot
   can be reclaimed before the parent calls `waitpid()`. A later focused
   variant forces that zombie writer through the post-checkpoint file-per-table
   DML marker path, proving the next ownerless opener can recover the committed
   row image and drain DML marker/WAL evidence before the parent reaps the
   child. A paired killed-uncommitted variant forces the same file-per-table
   DML path inside an explicit transaction before `COMMIT`, then proves no-live
   recovery drops the interrupted row image and does not persist native
   file-operation marker state. Durable
   rollback/recovery records are still needed before product writers can recover
   a crashed owner while other processes continue running.

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
   Ownerless write commits now publish transaction-owned dirty page images
   before releasing shared lock-registry entries. MTR-proven autocommit commits
   can publish the page-visible LSN directly from the durably synced
   page-version WAL, while DDL, unproved transaction-deferred dirty pages,
   rollback/deadlock cleanup, and any MTR publish skip or failure still flush
   dirty pages through the current native InnoDB log LSN before page-visible
   publication. Because
   the current implementation
   still uses one InnoDB buffer pool per process, the shared registry still has a
   page-level physical X resource for native lock records that have no record,
   gap, insert-intention, or supremum flags. Ordinary `REC_NOT_GAP` row locks
   keep their record identity, which avoids turning row-heavy transactions into
   broad same-page waits. A separate
   page-write lock-registry segment mirrors X/SX page-latch write ownership
   for B-tree and external-value pages with synthetic page-write resources so
   internal data page writes that do not surface as row locks still serialize
   across process-local buffer pools without being starved by row-lock-heavy
   transactions. In explicit/non-autocommit transactions, page-write locks
   acquired before native `trx_t::id` assignment now use the stable transient
   page-write transaction identity for the same defer-until-commit decision as
   native transaction IDs, so the first dirty user data/index page is held and
   published at the transaction boundary rather than the mini-transaction
   boundary while autocommit DDL/DML keeps the prior mini-transaction release
   behavior unless InnoDB assigns a native transaction ID. Ownerless
   mini-transactions also prepare later persistent user pages in an
   already-modified tablespace before X/SX page-linked access, so
   secondary-index navigation refreshes before using peer-modified page state
   without turning cross-table row deadlocks into page-write deadlocks.
   The `ownerless-page-write-timeout-retry` slice treats ownerless page-write
   lock timeouts in the MTR and buffer pre-read hooks as internal physical-page
   contention that refreshes and retries instead of poisoning InnoDB transaction
   error state or returning null for mandatory dictionary pages. SQL-visible
   ownerless pressure and statement-lock busy errors remain raised by the
   MyLite statement policy layer where they can be reported safely.
   Undo segment creation explicitly enters the
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
   Hook-build crash coverage kills an `ALTER TABLE ... AUTO_INCREMENT` writer
   after native high-watermark persistence but before ownerless dictionary
   finish, then verifies recovered implicit ID allocation through
   ownerless/native reopen and forced `.shm` rebuild. The
   `ownerless-autoinc-column-ddl-refresh` slice also covers adding a new
   `AUTO_INCREMENT PRIMARY KEY` column during an InnoDB table rebuild while an
   already-open ownerless peer is live, proving the peer refreshes the rebuilt
   definition and next implicit ID before inserting. When that peer first
   observes the newer dictionary generation, its next insert avoids the
   ownerless visible fast path and uses the conservative dirty-page flush path
   so rebuilt clustered-index pages are durable for later indexed lookups. The
   `ownerless-autoinc-primary-key-ddl-refresh` slice moves the primary key away
   from an existing AUTO_INCREMENT column while retaining a unique secondary
   index, verifies the live peer sees the replacement clustered key, and
   preserves InnoDB's no-reuse gap after a duplicate replacement-key write
   consumes an AUTO_INCREMENT value. The registry header now carries a shared
   native-checkpoint pending bit that is set only when an ownerless publish
   creates or raises a table high watermark; the final no-live ownerless close
   path drains that bit through the existing native checkpoint/reclaim flow
   before clearing it, so forced `.shm` rebuild seeds from native pages that
   include consumed AUTO_INCREMENT reservations without adding per-insert
   durable writes.
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
   serialization, same-row writer waits, gap-lock insert timeout plus
   post-release retry, savepoint rollback visibility before and after commit,
   serializable read locks blocking a peer writer,
   a two-process serializable write-skew candidate where both transactions
   cannot commit disjoint predicate-dependent updates,
   reverse-order table deadlocks, stale committed reads after an external write,
   mixed reader/writer processes, a bounded independent-table writer/reader
   stress loop, shared AUTO_INCREMENT assignment across concurrently opened
   ownerless insert workers, ownerless AUTO_INCREMENT DDL high-watermark
   refresh, ownerless AUTO_INCREMENT column-add rebuild refresh,
   ownerless AUTO_INCREMENT primary-key replacement refresh,
   ownerless AUTO_INCREMENT descending primary-key replacement refresh,
   consistent-snapshot retention without a preceding
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
   Page-index lookup distinguishes absent entries from incomplete-index and
   older snapshot states for diagnostics, and page reads still use the WAL scan
   as the authoritative proof when the index cannot prove the requested
   snapshot. Direct page-index reads and WAL scans run under the existing
   page-log read guard instead of taking a nested checkpoint read lock. The
   stats-enabled embedded performance probe classifies authoritative WAL-scan
   misses into true page-key absence versus same-page-not-visible misses, and
   classifies page-version publish append attempts by InnoDB page type and by
   native-support versus non-native-support class while preserving the legacy
   `snapshot_boundary` key for that complement and separately counting actual
   synthesized snapshot-boundary appends in the first-party hook stats. A
   process-local negative cache can skip repeated scans only after a stable WAL
   snapshot and authoritative scan proves same-page absence; a fresh page-index
   miss still scans the WAL before returning unavailable. A
   WAL-generation/covered-offset tail cache can extend true no-same-page proofs
   across unrelated page-index generation changes after scanning only the newly
   appended tail. Product ownerless opens
   add a shared page-version pin registry for explicit repeatable-read and
   serializable snapshot LSNs. `START TRANSACTION WITH CONSISTENT SNAPSHOT`
   publishes its page-version pin before executing the SQL so close-time
   reclamation cannot race the native snapshot boundary. Close-time page-log
   reclamation can run with live peers only when that registry reports zero
   active pins, after taking a nonblocking ownerless statement gate and proving
   shared native write/recovery state is idle before forcing the process-local
   InnoDB checkpoint. In-progress write/DDL statements or active transaction,
   InnoDB lock, page-write, dictionary, redo, or page-version pin state leave
   the WAL retained. Live-peer reclaim keeps checkpointable user data/index
   page records in WAL until no-live reclaim can make the native data file
   authoritative; non-DDL no-live DML reclaim can truncate those records only
   after native page proof. Boundary-preserving page-log primitives remain
   covered as lower-level evidence, but product close-time reclaim avoids native
   checkpoint side effects while a live peer can still need retained WAL.
   The page-version WAL format also has internal `FIL_PAGE_INDEX` and
   `FIL_PAGE_UNDO_LOG` delta payloads for repeated page identities. Delta
   records reference a
   durable standalone base record in the same page-log file, offset, and
   generation, are selected after one durable standalone base and only when
   they are less than half the standalone payload, and are rewritten as
   standalone records during checkpoint if retained. The append path uses the
   stable page image through the delta decision, checksum, payload write, and
   base-cache update, while system-tablespace index pages stay standalone so
   DDL/dictionary churn does not use the DML hot-page optimization. A reduced
   stats-enabled production probe over 100 ownerless autocommit inserts
   reduced index page-log payload from the preceding `1779` bytes/insert
   baseline to `598.580` bytes/insert with `90` delta records, and the later
   first-base warm-up slice reduced the current 500-row attribution shape from
   `927.956` to `829.066` index bytes per insert. A follow-up undo-log delta
   slice selected `0.752` undo-delta records per insert and reduced undo-log
   payload from `539.910` to `210.006` bytes per insert while preserving one
   rollback-segment and one undo history-proof publication per insert. The
   append performance probe now also attributes accepted fast versus exact
   index/undo deltas and fast/exact rejection reasons, separating the fast
   payload limit, standalone-size comparisons, and delta-build failures before
   any later fast-path rule change is considered. Exact fallback now reuses a
   fast-miss delta payload after standalone encoding proves it still beats the
   current standalone payload, avoiding duplicate delta construction without
   changing the delta acceptance rule; when an exact size-only standalone probe
   proves the retained delta wins, exact fallback also skips materializing the
   standalone payload that would otherwise be discarded. Successful exact-probed
   delta appends now refresh the volatile standalone-size estimate for that
   durable base, so later same-base appends can fast-accept without repeating
   the size probe. Exact fallback's size-only standalone probe now computes
   compact sparse, varint compact sparse, fill-sparse, and trailing-size
   evidence for `FIL_PAGE_INDEX` and `FIL_PAGE_TYPE_SYS` pages in one pass,
   preserving fill-sparse rejection of retained deltas without a second
   full-page scan. Page-log
   scan/replay/checkpoint validation now streams the full-page checksum for
   non-delta full, trailing-zero, sparse-zero, compact sparse, varint compact
   sparse, and fill-sparse records instead of reconstructing a full page just
   to prove payload integrity; delta records remain on the existing base-page
   reconstruction path. Broader native history-proof replacement,
   redo/checkpoint reconciliation, and DDL/file lifecycle recovery remain
   planned work.
   Parser-proven pure `INSERT ... VALUES` visible-fast-path statements with one
   through 65536 row constructors reuse one page-log append session across the
   statement's ownerless mini-transactions and release it before page-log
   sync/page-visible LSN publication. Focused SQL coverage keeps the existing
   native history WAL proof, capped multi-row visible-fast proof, single-row
   append-session proof, and conservative upsert fallback checks while asserting
   that append-session begin/end counts collapse for successful capped
   visible-fast inserts. The 65537-row boundary stays outside append batching
   and deferred latest-checkpoint coalescing. Later latest-only checkpoint
   updates inside the same implicit/autocommit deferred append-batch statement
   are coalesced only after the first successful latest-only checkpoint has
   been preserved; final durable page-visible checkpoint publication is
   unchanged. Broader DML, DDL, foreign-key shapes, row lists above 65536,
   single-row autocommit performance, and cross-statement group-commit batching
   remain future work.
   Undo, allocation,
   tablespace-header, extent, transaction-system, change-buffer, and system page
   records remain primitive evidence for future active-pin compaction.
   Unrecognized page types remain snapshot-sensitive. Dead-owner cleanup releases a killed
   reader's MDL, read-view, and page-version pin state so it does not starve
   later live-peer reclamation. No-live-process
   recovery applies visible page-version records into
   native tablespace files and retains complete committed WAL records until
   native redo/checkpoint
   reconciliation can prove that record reclamation is safe. Guarded ownerless
   SQL allows page-version reads for direct or prepared `SELECT`/`WITH`
   statements at a live page-version read LSN, including transactions with
   local writes whose own uncommitted redo can hold back the durable
   page-visible LSN. Eligible handles keep that read LSN monotonic and pin it
   before clean-page refresh. Plain reads inside explicit transactions that
   already performed a local write or locking read preserve local pages even
   before the handle has a local-native read watermark, so same-transaction
   reads continue to observe their own dirty rows. Direct successful reads
   retain that shared handle
   pin until a replacement read, non-read/current-read statement, error, or
   close, and live raw-latest promotion is blocked while other active native
   transactions or active redo reservations are present. An older external
   snapshot pin alone retains WAL for that reader without blocking unrelated
   current autocommit page-version reads once native transaction state is idle.
   Eligible autocommit page-version reads close the current
   InnoDB read view at statement start so a later statement can observe a new
   peer commit, and ownerless page-write hooks avoid page-write ownership for
   SQL `SELECT`, including locking reads such as `SELECT ... FOR UPDATE`, so
   native row-lock/current-read waits remain visible. Repeatable
   read and serializable transactions pin that live read LSN on their first
   consistent read, and `START TRANSACTION WITH CONSISTENT SNAPSHOT` pins it at
   transaction start. Read-committed explicit transactions remain non-pinning:
   each eligible read can advance to the live read LSN when the transaction has
   not performed local writes or locking reads and no other explicit ownerless
   transaction, shared read-write transaction, or redo reservation is active.
   Active transactions that cannot safely run a global refresh keep dirty local
   pages resident, and clean-page refresh skips locally dirty buffer pages.
   A new explicit transaction entering a data/index page write force-refreshes
   a dirty process-local page left by earlier work under the ownerless
   page-write lock unless that same transaction already modified the page or
   the single-owner proof can show that no external peer image can exist; the
   forced refresh may overlay a visible native disk page even when local page
   LSN ordering alone would not prove it newer, preventing stale full-page
   flushes from erasing peer commits on the same physical page.
   The per-space transaction page-write gate remains statement-scoped:
   explicit transactions release gate markers at statement end so unrelated
   writers in the same tablespace are not serialized for the transaction
   lifetime. Explicit non-autocommit preread and prepare paths do not add
   more transaction gates after a statement already holds one, and their
   additional clean page-write probes are untracked and nonblocking; a conflict
   returns without publishing shared-registry waiters and skips only the clean
   preread refresh, while a later real dirty path still acquires tracked
   page-write ownership and refreshes before marking the page dirty.
   Statement-end cleanup also releases transaction-deferred page-write records
   that never became dirty pages and never captured transaction page images.
   Real dirty page-write ownership remains transaction-scoped until
   commit or rollback after page-level ownerless acquisition records the
   modified page, so incomplete later callbacks cannot allow another explicit
   writer to interleave with a stale process-local page image for that page.
   InnoDB read completion
   validates ownerless page identity and checksum in a temporary buffer and
   overlays the disk frame only when the disk frame is invalid for the expected
   page or older by page LSN. After InnoDB startup completes, this includes
   `space_id=0` system-tablespace pages so dictionary flushes after local or
   peer DDL can reload unflushed dictionary records from the page-version WAL;
   startup and recovery keep this hook disabled while redo/log initialization
   is still in progress. The page-write entry hook also returns before
   ownerless lock acquisition, refresh, boundary publication, or page-write
   perf accounting during startup/recovery, so pre-start ownerless lifecycle
   hooks cannot affect native doublewrite or system-tablespace bootstrap
   writes. Non-forced page-version write refresh uses the same monotonic rule
   and does not overwrite a same-LSN or newer clean local page with a retained
   page-version image. Peer dictionary-generation refresh runs
   before page-version read eligibility is chosen. It refreshes page-0
   space-header flags using a max-sized page read so compressed row-format
   rebuilds can update the native `fil_space_t` page size, then keeps the
   already-open handle out of page-version table reads while the post-DDL
   conservative flag is set. This avoids overlaying retained `(space_id,
   page_no)` records from the pre-rebuild table image onto the rebuilt table,
   including compressed external BLOB page chains whose continuation pages can
   be invalid for the new table image. If the dictionary refresh itself is the
   first point where the handle notices the peer generation change, the same
   statement now revokes page-version-read eligibility before native execution;
   compressed row-format coverage asserts the post-peer parent window records
   refresh calls and zero `refresh_page_version_reads_enabled`.
   No-live-process
   recovery treats the page-version WAL as the visibility authority and applies
   the latest visible page-version record by the same commit-first ordering as
   page-version reads to existing native InnoDB tablespace files before
   rebuilding `.shm`, even when disk carries a higher-LSN page image left by a
   crashed uncommitted writer. It uses page-0 space-id discovery for existing
   single-file tablespaces. Product no-live recovery uses an explicit replay
   mode to skip retained page-version records for tablespaces that are no
   longer present, such as dropped and same-schema or cross-schema
   same-statement multi-dropped DDL stress tables, while the strict primitive
   replay API still fails closed on unresolved tablespaces. No-live `.shm`
   rebuilds checkpoint retained reader-boundary WAL instead of replaying it
   when their remaining state is stale read-view/page-pin evidence without
   native writer recovery evidence, and focused SQL coverage now verifies
   dropped and same-schema or cross-schema same-statement multi-dropped
   file-per-table absence,
   ordinary-created file-per-table final state with secondary-index
   metadata/use, LIKE-copy, and CTAS-created file-per-table final states,
   plus CTAS post-create DML while a stale reader pins page-version WAL,
   same-name recreated file-per-table final state with page-0 space-id identity
   checks, rename-away plus new original-name created dual file-per-table
   final state with page-0 space-id identity checks, cross-schema renamed
   file-per-table final state, truncated file-per-table post-truncate state,
   copy-style force-rebuilt, primary-key-rebuilt, and compressed
   row-format-rebuilt file-per-table final states, multi-pair rename-swap
   final state, and multi-table
   dropped-schema absence through ownerless/native reopen before and after
   forced `.shm` rebuild.
   Broader DML/DDL and reconstruction of missing DDL-created tablespaces still
   use the conservative native-file bridge until the page replay protocol
   carries durable file lifecycle metadata. The
   conservative bridge now advances
   the local durable LSN when a process reads an externally flushed page whose
   page LSN is ahead of the local log, and refreshes durable tablespace header
   and allocation metadata from page 0 plus the file-segment inode page after
   visible peer commits so native allocation bounds do not remain stale. While a
   dictionary DDL statement holds the ownerless dictionary statement lock, the
   InnoDB hook suppresses repeated internal external allocation-page refreshes
   after the pre-statement refresh has run; that prevents online DDL from
   rewinding its own freshly local file-per-table allocation state while peer
   writes are already excluded.
3. Publish commit end marks and reader snapshots.
   Guarded commits now separate raw redo progress from page-visible progress in
   the ownerless redo state segment. `redo_leave` still advances the raw latest
   LSN used to keep peer InnoDB redo state monotonic, but the page-visible LSN
   advances only after transaction-owned dirty pages up to that commit LSN have
   been published into the page-version log and the page-version log has been
   durably synced under the append range; MTR-proven autocommit commits and
   transaction-deferred dirty pages proven by transaction-page publication can
   skip the native dirty-page flush, while DDL, unproved
   transaction-deferred dirty pages, rollback/deadlock cleanup, and any MTR
   publish skip or failure still use the conservative native bridge. The slow
   path flushes native dirty pages through the current InnoDB log LSN before
   publishing that higher visible boundary, so peer refresh can use durable
   disk state instead of an ownerless WAL image.
   Page-visible publication is skipped while
   another live ownerless process is inside an explicit transaction, the shared
   transaction registry still has active read-write transactions owned by
   another process, or the page boundary would need a post-rollback global
   `log_get_lsn()` proof.
   Page-version WAL lookups capture a stable
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
   same byte-range lock file on every statement. A successful session
   `SET lock_wait_timeout = N` on an ownerless handle now also bounds these
   MyLite statement-lock waits to `N` seconds; handles that do not set the
   variable keep the existing 60 second internal wait. Contended
   directory-owned file-lock acquisition now polls at 1 ms first and backs off
   to the existing 10 ms cap, preserving the timeout contract while reducing
   short ownerless statement-lock and startup-lock handoff latency.
   External record waits use targeted waited-page refresh after the blocker
   releases. Page-version scans, rebuilds, and checkpoints ignore only the
   final incomplete or checksum-corrupt tail record; checksum failure before
   the tail is treated as corruption.
   Undo and system-page versions publish at mini-transaction scope after
   ownerless redo is written and the raw latest LSN is recorded, because
   cross-process MVCC readers may need active transactions' undo pages to build
   previous row versions before the writer commits. User data/index pages remain
   transaction-visible and are published only at commit/rollback visibility
   boundaries; dirty current-page images held under transaction-deferred
   page-write ownership are no longer published as mini-transaction boundary
   records. Ownerless mini-transactions therefore make pre-write preparation
   page-kind aware: once an explicit transaction has deferred user page writes,
   later undo and system-page writes still acquire ownerless page-write
   ownership before page-linked state is read or modified.
   First-page user data/index writes can happen before InnoDB assigns the
   native transaction ID, and later same-tablespace user pages can be read while
   InnoDB navigates clustered or secondary B-tree state. Ownerless
   transaction-scoped page-write release therefore recognizes
   `trx_t::mylite_ownerless_page_write_trx_id` as well as `trx_t::id` for
   explicit/non-autocommit transactions, and the prepare path runs for later
   pages in an already-modified tablespace rather than stopping after the first
   deferred user page. The
   `ownerless-transient-page-write-boundaries` slice covers the resulting
   clustered/secondary index atomicity regression through repeated foreign-key
   graph stress. The `ownerless-transaction-page-lsn-coverage` slice then keeps
   the transaction page publication boundary at least as new as the tracked
   page images themselves, so a secondary-index page whose MTR LSN outruns the
   initial transaction commit LSN is not skipped before ownerless page-write
   locks are released. The same slice treats DML and locking reads as
   current-read refresh points, so clean stale local pages are evicted or
   refreshed before an `UPDATE ... WHERE ...` search can silently miss a
   peer-committed row inside an explicit transaction. The monotonic visibility
   fence keeps that coverage per page: a transaction-owned page whose observed
   `FIL_PAGE_LSN` outruns the commit boundary is published at that page LSN,
   but the global page-visible LSN is not promoted beyond the transaction
   boundary.
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
   statement; ordinary statements keep those gates per tablespace so independent
   table writers are not promoted to a global physical-page wait only because a
   clean preread or metadata path touched another tablespace. Explicit
   transaction preread/prepare paths that already have a statement gate use
   untracked zero-timeout page-write probes for additional clean pages; conflict
   returns do not publish shared-registry waiters. Statement-end cleanup
   releases both gate markers and clean page-write records that never became
   dirty or image-backed. Real dirty page-write locks stay held until commit or
   rollback after page-level acquisition records the modified pages.
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
   primitive replay fails closed when the tablespace cannot be resolved or when
   duplicate page-0 FSP-header candidates make the target ambiguous; product
   no-live recovery uses an explicit mode to skip unresolved retained records
   for tablespaces no longer present in the directory or otherwise ambiguous.
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
   Page-visible publication now first durably syncs the page-version WAL under a
   safe serialized sync point, except that a process-local clean-sync anchor may
   elide the filesystem sync when both the WAL size and page-log header
   generation are unchanged from an already-synced image. The stats-enabled
   performance probe splits
   that visible-anchor path into page-log sync lock/header/data-sync costs plus
   durable `.ckpt` update lock/read/write/data-sync costs before any later
   batching or deferral is considered and reports clean-sync skips separately.
   Capped visible-fast append-batched statements also preserve the first
   latest-only `.ckpt` update and coalesce later non-durable latest-only
   updates in the same statement for implicit/autocommit writes and proven
   explicit-transaction insert writes, while savepoint-disqualified explicit
   transaction statements, unsafe fault hooks, and final durable latest/visible
   checkpoint publication keep the previous behavior.
   The redo segment bookkeeping now lives
   in a first-party primitive that owns latch/refcount handling, latest/visible LSN
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
   The commit-race harness now separates directory open/start serialization
   from the update and commit race, then treats combined ownerless InnoDB-lock
   and page-write waiters as accounted-for workers before releasing the commit
   pipe. This avoids artificial parent-side deadlocks while preserving the
   same committed-delta and reopen oracles; shutdown cleanup records whether
   ownerless hooks were installed during the InnoDB lifetime so ownerless
   `TRX_UNDO_TO_PURGE` descriptors are still cleaned after callback reset
   while active/prepared leftovers remain assertions.
   The deterministic row-deadlock helper now takes the first row lock with
   `SELECT ... FOR UPDATE` before its ready barrier and performs both DML
   updates after releasing both children together, preserving the one-winner
   and one-victim invariant while accepting MariaDB 1213 or 1205 victim
   outcomes under ownerless scheduling. The explicit-DML marker discard path
   verifies 1213 victims are clean after MyLite's internal deadlock rollback
   and 1205 victims are clean after the explicit test rollback, matching
   MariaDB's lock-wait-timeout transaction lifetime. Deadlock and rollback
   handoff now publish the current post-undo page image as the page-visible
   boundary before releasing ownerless page-write locks for local SQL
   rollback/deadlock handoff, but recovered rollback during ownerless native
   startup is not published as ownerless-visible while a peer transaction can
   still be live.
   Waited record/page-write/current-read refreshes consume the visible boundary
   rather than raw latest redo, and startup visible-boundary page imports bypass
   the ownerless page-write protocol so open-time refresh cannot wait on a live
   writer's transaction-scoped page-write locks.
   Cross-process group commit remains an optimization candidate rather than
   claimed behavior.
4. Reconcile InnoDB redo with MyLite page-version visibility.
   Generic tablespace replay still treats the page-version WAL image as
   authoritative and primitive coverage rewrites a same-LSN different-image
   page. Product no-live replay uses an explicit equal-LSN native-page guard
   because retained page-version WAL can be a snapshot boundary rather than the
   newest native side-effect image. Non-forced page-version write refresh
   accepts newer page-version images, but does not rewind a same-LSN or newer
   clean local page to an older page-version image;
   focused CTAS post-create DML coverage exercises the case where a retained
   stale-reader boundary page is older than the populated CTAS data page while
   later `UPDATE`, `DELETE`, and `INSERT ... SELECT` statements mutate that
   destination.
   Ordinary native exclusive
   read/write opens now keep page-version reads enabled when retained WAL
   payload records exist, and no-live-process replay retains complete
   page-version WAL records, so covered concurrent explicit ownerless commits
   remain visible through `MYLITE_OPEN_READWRITE` before and after forced
   `.shm` rebuild. Product no-live replay also skips
   retained page-version records whose tablespace no longer exists, covering
   dropped and same-schema or cross-schema same-statement multi-dropped DDL
   stress tables without treating stale `.shm` state as durable truth; no-live
   stale-reader `.shm` rebuilds
   checkpoint retained reader-boundary WAL before segment rebuild, with focused
   dropped, same-schema and cross-schema same-statement multi-dropped,
   renamed, truncated, force-rebuilt, primary-key-rebuilt, and compressed
   row-format-rebuilt file-per-table, same-schema and
   cross-schema multi-rename swap, and multi-table schema-drop SQL coverage.
   Native InnoDB redo/checkpoint reconciliation is still incomplete:
   MyLite now reclaims retained page-version records on non-read-only runtime
   close after forcing a native InnoDB checkpoint, advancing local native LSN
   state to the durable page-visible LSN when needed, and, when no live peers
   remain, publishing eligible native support/allocation/system buffer-pool
   pages and flushing native dirty pages to advance a lagging page-visible LSN
   to a newer raw latest LSN before checkpoint proof; if shared redo
   publication is still capped by the native
   checkpoint-record gap, no-live close persists the newer page-visible LSN
   only after native checkpoint coverage proves it. It then refreshes external
   clean page state, proves the native checkpoint covers that durable visible
   LSN according to MariaDB's checkpoint-record rule, compacting records at or
   below that safe LSN while retaining newer complete records, and replacing
   the page-version index before checkpoint locks are released. With live peers,
   this path is gated by a nonblocking ownerless statement gate, the shared
   page-version pin registry, and native write/recovery-idle proof, and runs
   only when no active page-version pins remain. Page-version publication now
   opportunistically synthesizes a boundary record from the native tablespace
   page when an older snapshot pin is active, no WAL boundary exists, and the
   native page LSN is at or below the oldest pin; if that proof is unavailable,
   missing data-page boundaries still conservatively leave the WAL unchanged.
   Ordinary exclusive read/write reopen with retained ownerless page-version
   WAL or a nonzero ownerless checkpoint-visible boundary force-refreshes clean
   process-local InnoDB buffer-pool pages from that boundary before SQL
   execution, so same-process embedded restart cannot reuse stale clean pages
   left by an earlier ordinary open even after peer-close checkpointing has
   compacted the page-version WAL. Ownerless handles use the same no-skip
   clean-page refresh when they first observe a new shared process generation
   or replace an older retained handle pin with a newer page-version read LSN.
   Eligible retained reads keep native clean-page refresh active but reject
   lower visible-boundary overlays for user data/index/blob pages when the
   overlay would downgrade the handle below its retained page-version read LSN;
   current live ownerless page images can still advance those pages, and native
   support pages continue to refresh through the visible boundary.
   Live-peer reclaim also stays disabled for a writer runtime that has local
   writes but has not consumed the current visible page-version WAL after those
   writes, avoiding an immediate cleanup race with a peer's native page
   refresh. Live-peer reclaim can compact native-support-only page-version
   records after the live-peer gate proves no active pins or native write state,
   but keeps user data/index page-version records retained; no-live writer
   reclaim can still run when native page proof covers those records.
   Ownerless `COMMIT` and full `ROLLBACK` ending an explicit transaction with
   local writes take the global ownerless write statement lock and refresh
   current shared native state before executing, so independent process-local
   InnoDB support-page images cannot hide a peer's concurrent commit evidence.
   Full rollback remains conservative after native undo: it releases native and
   ownerless locks but does not publish transaction-tracked user page images,
   flush transaction page-write pages, or force-advance the page-visible LSN as
   a committed boundary. The `ownerless-random-tx-rollback-handoff` slice adds a
   three-round random transaction stress guard for full-rollback and retry-attempt
   leakage.
   If that global statement byte is held by a peer writer that is waiting on
   this transaction's shared InnoDB or page-write lock, the transaction end can
   proceed after the shared registry proves the blocker relationship, allowing
   native `COMMIT`/full `ROLLBACK` to release the peer wait instead of timing
   out behind the outer statement gate.
   Read-only explicit transactions that only used native locking reads still
   avoid global refresh while active, but their transaction-end SQL is not
   queued behind a peer writer's global ownerless statement gate; native InnoDB
   row/table locks provide the wait and release semantics for those reads.
   Product close-time reclaim does not use the single-active-pin primitive while
   a live pin remains; it retains the WAL until release, then uses native
   checkpoint proof through the existing zero-pin or no-live reclaim path.
   Bounded SQL-level repeated same-row pressure and
   distinct large-row expanding-page pressure are now covered while a live
   repeatable-read snapshot pin remains active. The
   `ownerless-active-reader-pressure-limit` slice adds the first user-visible
   pressure policy: an opt-in `mylite_open_config` soft byte cap that returns
   `MYLITE_BUSY` before direct or prepared ownerless write execution when
   active pins retain page-version WAL at or above the configured limit,
   including focused prepared `UPDATE` and prepared `INSERT ... SELECT`
   dispatch coverage.
   The `ownerless-pressure-write-class-policy` slice broadens this evidence to
   representative direct `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`,
   `ALTER TABLE`, and `DROP TABLE` statements, while confirming `SELECT` and
   transaction-control statements remain available under pressure and blocked
   writes leave rows and metadata unchanged. The
   `ownerless-pressure-write-variant-policy` slice adds variant spelling
   coverage for `REPLACE`, `INSERT ... SELECT`, multi-table `UPDATE`,
   multi-table `DELETE`, `CREATE INDEX`, `DROP INDEX`, `RENAME TABLE`, and
   `TRUNCATE TABLE`, proving the same active-reader soft cap blocks those
   mutations before native row, index, rename, or truncate state changes.
   The `ownerless-ctas-pressure-policy` and
   `ownerless-ctas-insert-pressure-policy` slices add focused evidence that
   post-create `UPDATE`, `DELETE`, and `INSERT ... SELECT` against an existing
   CTAS destination are blocked before execution while retained active-reader
   WAL is at the soft cap, leave the CTAS rows unchanged under pressure, and
   then succeed after the snapshot pin releases.
   The `ownerless-pressure-replacement-copy-policy` slice adds focused
   evidence that `CREATE OR REPLACE TABLE ... LIKE` and
   `CREATE OR REPLACE TABLE ... AS SELECT` return `MYLITE_BUSY` before old
   replacement targets are dropped or copied metadata becomes visible under the
   same retained-WAL pressure, then succeed after reader release and survive
   ownerless/native reopen plus forced `.shm` rebuild.
   The `ownerless-pressure-generated-column-policy` and
   `ownerless-pressure-generated-column-fk-policy` slices add generated-column
   ALTER, generated-column secondary-index, and stored generated-column
   foreign-key ADD/DROP pressure coverage, proving those native metadata
   families return `MYLITE_BUSY` before MariaDB mutates table definitions while
   retained WAL is at the soft cap.
   The `ownerless-pressure-unsupported-policy-order` slice proves the same
   pressure cap does not mask explicit unsupported ownerless SQL diagnostics:
   representative table-admin SQL, SQL locked-table mode, flush read-lock or
   export SQL, host-file import SQL, tablespace detach/import SQL, and rejected
   storage-option DDL still return `MYLITE_ERROR` with their policy messages
   while retained WAL is already at the configured pressure limit. Follow-up
   pressure-order coverage proves direct and prepared event DDL/metadata plus
   scheduler variable assignments keep the server-surface diagnostic under the
   same retained-WAL pressure and leave no `information_schema.events` rows, and
   that top-level sequence DDL/value SQL keeps the sequence policy diagnostic
   before pressure handling or prepared-statement allocation without advancing
   sequence/default-table state. The `ownerless-pressure-partition-policy`
   follow-up proves partitioned-table DDL keeps the ownerless partition policy
   diagnostic under the same retained-WAL pressure and leaves rejected
   table/partition metadata absent through the existing reopen checks.
   The `ownerless-pressure-host-file-export-policy` slice adds representative
   direct `SELECT ... INTO OUTFILE`, direct `SELECT ... INTO DUMPFILE`, CTE
   export spelling, and prepared export checks proving host-file export SQL also
   keeps the server-surface diagnostic under that same retained-WAL pressure.
   The `ownerless-pressure-diagnostics` slice exposes the same active pin
   count, oldest pin LSN, raw WAL byte count, configured limit, and
   throttle-reached state through `mylite_ownerless_pressure_status()`.
   The `ownerless-no-live-pressure-reclaim-advance` slice adds deterministic
   SQL coverage for no-live close-time reclaim when the durable raw latest LSN
   is newer than the page-visible LSN while page-version WAL is retained. The
   selector now also deletes the checkpointed page-version WAL before ordinary
   native reopen and asserts `mylite_ownerless_innodb_checkpoint_covers_lsn()`
   for the reclaimed visible LSN, proving that this pressure-specific advance
   does not depend on retained WAL after no-live reclamation.
   The `ownerless-statement-checkpoint-scheduling` slice adds thresholded
   ownerless write/DDL/transaction-end statement-boundary scheduling for the
   no-live native reclaim path when no peer process is open. The
   `ownerless-live-peer-statement-checkpoint-gating` slice adds negative
   coverage proving live writer or active snapshot-pin state keeps WAL retained
   until close-time reclaim can pass the existing live-peer gate. The
   `ownerless-live-peer-statement-checkpoint-scheduling` slice now proves the
   same thresholded statement-boundary path can reclaim native-support-only WAL
   while an idle live peer is open when there are no active page-version pins or
   native write/recovery state; user page-version WAL retention remains covered
   by live writer, snapshot-pin, and active-reader pressure tests.
   The `ownerless-timer-checkpoint-scheduling` slice adds a MariaDB-registered
   runtime-owned scheduler that wakes independently of SQL execution and reuses
   the same native reclaim path when the writer runtime is idle, no active
   same-process statement, prepared result cursor, or explicit transaction is
   open, the WAL threshold is reached, and page-version pins have drained.
   Focused SQL coverage keeps a writer handle open, releases a shared read-only
   snapshot pin, executes no further writer SQL, and requires WAL checkpointing
   before close; the `ownerless-timer-prepared-result-gating` follow-up leaves
   an ownerless prepared `SELECT` result cursor active across the snapshot-pin
   release, proves the timer keeps WAL retained while the cursor is live, then
   finalizes the cursor and requires timer checkpointing without another writer
   SQL statement. Live-reclaim gating mirrors a process-local
   explicit-transaction count into each ownerless process slot so idle peers
   that are between SQL statements inside an explicit transaction still block
   native checkpoint reclamation until the transaction ends.
   The `ownerless-native-checkpoint-reclamation`,
   `ownerless-partial-page-log-reclamation`,
   `ownerless-live-reclaim-gating`, `ownerless-active-pin-reclaim`,
   `ownerless-native-boundary-synthesis`,
   `ownerless-active-reader-pressure-limit`,
   `ownerless-pressure-write-class-policy`,
   `ownerless-pressure-write-variant-policy`,
   `ownerless-pressure-diagnostics`,
   `ownerless-active-reader-pressure`, and
   `ownerless-no-live-pressure-reclaim-advance` slices record the source-backed
   boundaries for that reclamation work. The
   `ownerless-expanding-page-pressure` slice adds bounded SQL evidence for
   distinct large-row page sets under the same active-reader pin policy. The
   `ownerless-blob-page-pressure` slice adds focused SQL evidence for
   `ROW_FORMAT=DYNAMIC` `LONGBLOB` off-page native BLOB pages under a live
   snapshot pin, including `.ibd` page-type evidence, WAL retention during the
   pin, checkpoint after release, and ownerless/native reopen before and after
   forced `.shm` rebuild. The `ownerless-blob-page-size-matrix` slice broadens
   the same dynamic-row-format lifecycle evidence to bounded 12 KiB, 24 KiB,
   48 KiB, 96 KiB, and 192 KiB `LONGBLOB` payload sizes under one live snapshot
   pin. The `ownerless-blob-page-wide-size-matrix` slice records the widened
   local evidence while leaving exhaustive long-value limits to later stress. The
   `ownerless-compressed-blob-page-pressure` slice
   adds the same lifecycle evidence for `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`
   `LONGBLOB` values that create native `FIL_PAGE_TYPE_ZBLOB`/`ZBLOB2` pages,
   `ownerless-compressed-blob-page-size-matrix` broadens the compressed
   long-value size evidence to bounded 12 KiB, 24 KiB, 48 KiB, 96 KiB, and
   192 KiB payloads at `KEY_BLOCK_SIZE=8`, and
   `ownerless-compressed-blob-key-block-matrix` broadens that evidence to a
   bounded 1 KiB / 2 KiB / 4 KiB / 8 KiB / 16 KiB compressed key-block matrix.
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
   exclusive reopen. The no-live native checkpoint cutover selector also deletes
   checkpointed page-version WAL after bulk DML reclaim and verifies ordinary
   native reopen plus `mylite_ownerless_innodb_checkpoint_covers_lsn()` for the
   reclaimed visible LSN.

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
   creates, idempotently no-ops, replaces, drops, and ALTER-adds/drops a
   multi-column unique index from another ownerless process, verifies an
   already-open peer observes `NON_UNIQUE = 0`, reports duplicate key-name
   errno 1061 for plain duplicate unique-index creates/adds, preserves the
   active key definition across idempotent no-op branches, rejects duplicate
   writes on the active unique key definition, accepts the formerly duplicate
   old-key shape after replacement, and accepts the formerly duplicate
   replacement-key shape after the index is dropped. Standalone
   idempotent index coverage creates a
   secondary index with `CREATE INDEX IF NOT EXISTS`, verifies duplicate
   non-idempotent create errno 1061, preserves the original indexed column after
   a duplicate idempotent create, replaces the same index name with
   `CREATE OR REPLACE INDEX` over another column, and drops the index with
   repeated `DROP INDEX IF EXISTS`; it also verifies ordinary inline
   `CREATE TABLE ... INDEX` metadata and duplicate inline key-name errno 1061
   without a leaked failed table. Hook-build index-idempotent crash coverage now
   kills duplicate top-level `CREATE INDEX IF NOT EXISTS`, missing top-level
   `DROP INDEX IF EXISTS`, duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS`,
   and missing `ALTER TABLE ... DROP INDEX IF EXISTS` no-op writers after MariaDB
   returns success but before ownerless dictionary finish, then verifies original
   key-part preservation, missing-index absence, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild.
   Primary-key coverage now verifies initial
   peer-visible `PRIMARY(id)` metadata, duplicate plain primary-key add errno
   1068, `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS (code)` no-op
   preservation of the initial key and duplicate-key enforcement, then performs
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)` from another
   ownerless process, verifies an already-open peer observes `PRIMARY` on the
   replacement column, rejects a duplicate replacement-key write, accepts a
   duplicate of the old key column, and verifies the final clustered-index
   metadata through ownerless/native reopen before and after forced `.shm`
   rebuild. Hook-build primary-key idempotent crash coverage kills duplicate
   `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS (code)` after MariaDB success
   but before ownerless dictionary finish, then verifies recovered `PRIMARY(id)`
   preservation, `PRIMARY(code)` absence, duplicate-id rejection, duplicate-code
   allowance, ownerless/native reopen, and forced `.shm` rebuild. Descending
   primary-key replacement coverage now performs
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code DESC)`, verifies an
   already-open peer observes `PRIMARY` on the replacement column with
   `COLLATION = 'D'`, rejects duplicate replacement-key writes, accepts a
   duplicate of the old key column, and verifies final clustered-index metadata
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Composite direction primary-key replacement coverage now performs
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (tenant_id ASC, code DESC)`,
   verifies an already-open peer observes both replacement key parts with
   `COLLATION = 'A'`/`'D'`, rejects duplicate composite-key writes, accepts a
   duplicate of the old key column, and verifies final clustered-index metadata
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Hook-build crash coverage kills the same composite direction primary-key
   replacement after native clustered-key rebuild but before ownerless
   dictionary finish, proves live-peer cleanup remains busy until no-live
   recovery, and verifies the same final metadata/enforcement through
   ownerless/native reopen and forced `.shm` rebuild. This
   proves the current dictionary-generation serialization and
   pre-statement refresh path for the representative create/alter/index
   allocation, replacement, unique-index enforcement, and primary-key
   replacement cases. AUTO_INCREMENT DDL coverage raises and then lowers the
   table option from one ownerless process while an already-open peer inserts
   implicit IDs, verifying peer-visible high-watermark refresh plus
   ownerless/native reopen before and after forced `.shm` rebuild. Hook-build
   AUTO_INCREMENT DDL crash coverage kills a representative
   `ALTER TABLE ... AUTO_INCREMENT` writer after native high-watermark
   persistence but before ownerless dictionary finish, then verifies recovered
   implicit ID allocation through ownerless/native reopen and forced `.shm`
   rebuild; the
   AUTO_INCREMENT column DDL slice adds a rebuild-style `ADD COLUMN ... PRIMARY
   KEY` case and verifies the already-open peer sees the new column and next
   implicit ID through ownerless/native reopen before and after forced `.shm`
   rebuild, including primary-index lookups after the peer inserts under the
   refreshed dictionary. AUTO_INCREMENT primary-key replacement coverage
   preserves a unique secondary index on the AUTO_INCREMENT column while moving
   `PRIMARY` to `code`, verifies the already-open peer receives the next ID,
   and verifies a failed duplicate replacement-key insert leaves a non-reused
   AUTO_INCREMENT gap across forced `.shm` rebuild. AUTO_INCREMENT descending
   primary-key replacement coverage keeps the same allocation proof while moving
   `PRIMARY` to `code DESC`, verifies `COLLATION = 'D'` metadata, and verifies
   the duplicate-key allocation gap across forced `.shm` rebuild.
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
   generated-column ALTER add/drop and same-kind expression-replacement refresh,
   generated-column secondary-index create/use/drop refresh including prefix
   key parts, indexed generated-column expression replacement,
   indexed generated-column expression duplicate-key policy,
   generated-column primary-key rejection policy, generated-column
   nondeterministic-expression policy,
   table-wide character-set conversion from `latin1` to `utf8mb4`,
   row-format rebuild from `COMPACT` to `DYNAMIC`,
   table comment metadata changes,
   `ALTER TABLE ... FORCE` rebuild,
   column-default SET/DROP metadata and peer DML effects,
   column idempotent ADD/DROP metadata and default-preservation effects,
   hook-build column-idempotent crash recovery for duplicate add and missing
   drop no-op branches,
   hook-build column missing-`IF EXISTS` crash recovery for missing modify,
   rename, change, and default no-op branches, including generated-column/CHECK
   expression preservation for missing rename, change, and default no-ops,
   an online/in-place index alter variant,
   column-shape ALTERs that add, modify, rename, and drop columns,
   explicit InnoDB instant ADD/DROP/reorder column metadata,
   instant FIRST/AFTER stored-column placement including `LOCK=DEFAULT`,
   `LOCK=SHARED`, and `LOCK=EXCLUSIVE` variants, instant column rename, virtual
   generated-column add/drop including `LOCK=SHARED`/`LOCK=EXCLUSIVE`
   variants, `CREATE TABLE ... LIKE`, and
   `CREATE TABLE ... SELECT`. Hook-build crash coverage now kills
   representative `CREATE TABLE ... LIKE` and CTAS writers after native
   destination table creation but before ownerless dictionary finish. The LIKE
   and focused no-definition-list CTAS selectors now prove live-peer cleanup can
   finish the dead dictionary generation while another ownerless peer remains
   open; coverage verifies recovered native files, table/column metadata,
   copied `LIKE` index metadata, CTAS rows, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild.
   Table idempotent DDL coverage now exercises
   `CREATE TABLE IF NOT EXISTS`, duplicate-create errno 1050 for the
   non-idempotent spelling, no-op duplicate preservation of the original table
   definition, `CREATE OR REPLACE TABLE` replacement of the existing native
   InnoDB `.frm`/`.ibd` table definition and rows, missing-table
   `DROP TABLE IF EXISTS`, repeated real-table drop, native `.frm`/`.ibd`
   absence, and final ownerless/native reopen before and after forced `.shm`
   rebuild. Hook-build crash coverage now kills a representative
   `CREATE OR REPLACE TABLE` writer after native old-table replacement but
   before ownerless dictionary finish, then verifies recovered replacement
   native files, old metadata absence, new metadata, empty replacement rowset,
   post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild.
   Hook-build after-drop crash coverage now also kills a representative
   `CREATE OR REPLACE TABLE` writer after MariaDB removes the old native target
   and before the replacement `.frm`/engine table is created, then verifies
   ownerless/native reopen see the absent table, forced `.shm` rebuild
   preserves absence, and the same SQL name can be recreated with fresh
   metadata and rows.
   Hook-build crash coverage now also kills representative
   `CREATE OR REPLACE TABLE ... LIKE` and
   `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy writers after
   native completion but before ownerless dictionary finish, then verifies
   copied replacement metadata, old metadata absence, copied `LIKE` secondary
   index metadata, CTAS copied rows, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild.
   No-live stale-reader replay coverage also verifies same-name
   `CREATE OR REPLACE TABLE` replacement preserves the new file-per-table
   tablespace, replacement schema, replacement rows, and page-0 space identity
   through ownerless/native reopen before and after forced `.shm` rebuild.
   A focused CTAS replacement replay selector also verifies
   `CREATE OR REPLACE TABLE ... AS SELECT` preserves the populated replacement
   tablespace, source table, CTAS rows, post-create DML rows, and page-0 space
   identity through ownerless/native reopen before and after forced `.shm`
   rebuild.
   A focused LIKE replacement replay selector verifies
   `CREATE OR REPLACE TABLE ... LIKE` preserves the copied replacement
   tablespace shape, copied secondary-index metadata, source table, inserted
   replacement rows, and page-0 space identity through ownerless/native reopen
   before and after forced `.shm` rebuild.
   Hook-build crash coverage also kills duplicate
   `CREATE TABLE IF NOT EXISTS` and missing `DROP TABLE IF EXISTS` no-op
   writers after MariaDB returns success but before ownerless dictionary finish,
   then verifies the original real table definition or missing-table absence is
   preserved through ownerless/native reopen and forced `.shm` rebuild.
   Hook-build column-idempotent crash coverage kills duplicate
   `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing
   `ALTER TABLE ... DROP COLUMN IF EXISTS` no-op writers after MariaDB returns
   success but before ownerless dictionary finish, then verifies original
   column/default preservation, missing-column absence, plain duplicate-add
   errno 1060, plain missing-drop errno 1091, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild.
   Hook-build column missing-`IF EXISTS` crash coverage kills missing
   `ALTER TABLE ... MODIFY COLUMN IF EXISTS`, missing
   `ALTER TABLE ... RENAME COLUMN IF EXISTS`, missing
   `ALTER TABLE ... CHANGE COLUMN IF EXISTS`, missing
   `ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and missing
   `ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` no-op writers after
   MariaDB returns success but before ownerless dictionary finish, then verifies
   original column/default preservation, missing and attempted renamed/changed
   column absence, plain retry errno 1054, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild. Focused
   expression-table variants kill missing `RENAME COLUMN IF EXISTS`,
   `CHANGE COLUMN IF EXISTS`, and `ALTER COLUMN IF EXISTS SET/DROP DEFAULT`,
   then verify the real column, stored and virtual generated expressions, real
   defaults, CHECK enforcement, missing attempted names, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild remain unchanged.
   The broader DDL and instant-variant selectors
   now close all ownerless peers and verify the final state through no-live
   ownerless read/write reopen, ordinary exclusive read/write reopen, forced
   `.shm` deletion plus ownerless rebuild, and ordinary exclusive reopen after
   that rebuild. Schema lifecycle coverage now creates a schema and InnoDB
   table from one ownerless process, verifies an already-open peer observes and
   writes through the new schema, drops the schema from the DDL process, and
   verifies peer-visible absence plus ownerless/native reopen before and after
   forced `.shm` rebuild. Hook-build schema-create crash coverage now kills a
   `CREATE DATABASE` writer after native schema directory/`db.opt` creation
   but before ownerless dictionary finish and verifies recovered schema
   defaults, post-recovery table creation, ownerless/native reopen, and forced
   `.shm` rebuild. Hook-build schema-alter crash coverage now kills an
   `ALTER DATABASE` writer after native `db.opt` rewrite but before ownerless
   dictionary finish and verifies recovered schema defaults, pre-alter table
   collation preservation, post-recovery table default inheritance,
   ownerless/native reopen, and forced `.shm` rebuild. Stale-reader
   schema-drop replay coverage now verifies
   retained reader-boundary WAL for multiple tables inside a dropped schema is
   checkpointed during no-live rebuild without recreating schema metadata,
   table metadata, the schema directory, or table files. Hook-build schema-drop
   crash coverage now kills a `DROP DATABASE` writer after native schema/table
   removal but before ownerless dictionary finish and verifies no-live
   ownerless/native reopen of the absent schema before and after forced `.shm`
   rebuild. Schema default DDL
   coverage now creates a schema with
   explicit default charset/collation, verifies native `db.opt` presence, runs
   `ALTER DATABASE` from another ownerless process, verifies an already-open
   peer observes the changed defaults and that later tables inherit them, drops
   the schema, and verifies final absence through ownerless/native reopen before
   and after forced `.shm` rebuild. Schema idempotent DDL coverage now exercises
   `CREATE SCHEMA IF NOT EXISTS`, duplicate `CREATE DATABASE IF NOT EXISTS`
   without rewriting existing defaults, absent `DROP SCHEMA IF EXISTS`,
   existing-schema drop, and final absent-schema ownerless/native reopen before
   and after forced `.shm` rebuild. Hook-build schema-idempotent crash coverage
   now kills duplicate `CREATE DATABASE IF NOT EXISTS` and missing
   `DROP SCHEMA IF EXISTS` no-op writers after MariaDB returns success but
   before ownerless dictionary finish, then verifies original schema defaults,
   real schema/table preservation, missing-schema absence, ownerless/native
   reopen, and forced `.shm` rebuild. Cross-schema rename coverage now creates an
   InnoDB table in `app`, writes through it from an already-open peer, renames
   it into a second schema from the DDL process, verifies peer-visible source
   absence
   and target readability/writeability, checks `.frm` and `.ibd` movement
   between schema directories, and verifies final ownerless/native reopen before
   and after forced `.shm` rebuild. Multi-rename-cycle coverage now executes a
   three-pair `RENAME TABLE` swap in one statement, verifies an already-open
   peer sees row contents under the swapped names, checks that InnoDB `SPACE`
   identities swap with the table names, and verifies final ownerless/native
   reopen before and after forced `.shm` rebuild. Hook-build cross-schema
   multi-rename crash coverage now kills the same three-pair swap shape after
   native file movement but before ownerless dictionary finish, verifies
   live-peer cleanup remains busy until no-live recovery, and checks swapped
   InnoDB `SPACE` identities, final schema-directory files, post-recovery
   writes, ownerless/native reopen, and forced `.shm` rebuild. View metadata
   coverage now creates and queries a simple view over an InnoDB base table
   from one ownerless process,
   verifies that an already-open peer observes the view and base-table changes
   through it, drops the view, and verifies final view absence plus base-table
   durability through ownerless/native reopen before and after forced `.shm`
   rebuild.
   View crash coverage now kills simple `CREATE VIEW` and `DROP VIEW` writers
   after native view definition-file creation/removal but before ownerless
   dictionary finish, then verifies recovered present/absent view metadata,
   `.frm` file state, view query behavior, base-table writes, and
   ownerless/native reopen before and after forced `.shm` rebuild.
   Hook-build view replacement crash coverage now kills
   `CREATE OR REPLACE VIEW` and `ALTER VIEW` writers after native view
   definition rewrite but before ownerless dictionary finish, then verifies
   recovered replacement/altered metadata, old exposed-column rejection, new
   projection query behavior, base-table writes, and ownerless/native reopen
   before and after forced `.shm` rebuild.
   View variant coverage now verifies that an already-open peer
   observes `CREATE OR REPLACE VIEW` and `ALTER VIEW` definition changes from
   another ownerless process before final drop and ownerless/native reopen
   checks. View check-option coverage now verifies an already-open peer
   observes `WITH CASCADED CHECK OPTION` and `WITH LOCAL CHECK OPTION`
   metadata after create/replace/alter, performs valid DML through the
   updatable view, receives MariaDB errno 1369 for invalid insert/update
   attempts, and verifies final view absence plus base-table durability through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Prepared view DML coverage now verifies an already-open peer prepares
   `SELECT`, `INSERT`, and `UPDATE` statements through an updatable
   check-option view created by another ownerless process, receives MariaDB
   errno 1369 for invalid prepared insert/update attempts, reuses the prepared
   DML statements after a peer replaces the view predicate, and verifies final
   view absence plus base-table durability through ownerless/native reopen
   before and after forced `.shm` rebuild.
   Non-updatable view diagnostics coverage now verifies an already-open peer
   observes aggregate-view `IS_UPDATABLE = 'NO'` metadata, receives MariaDB
   errno 1471 for `INSERT` and errno 1288 for `UPDATE`/`DELETE` through the
   view, receives errno 1368 for `WITH CHECK OPTION` on a non-updatable
   replacement, observes a later non-updatable view replacement from another
   process, and verifies failed writes leave the InnoDB base table unchanged
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Prepared non-updatable view diagnostics coverage now verifies
   `mylite_prepare()` succeeds for eligible ownerless prepared DML and
   `mylite_step()` reports MariaDB errno 1471 for prepared `INSERT` plus errno
   1288 for prepared `UPDATE`/`DELETE` against both the original aggregate view
   and a peer-replaced aggregate view, with no base-table mutation before final
   ownerless/native reopen checks.
   Invalid view dependency coverage now verifies an already-open peer observes
   a valid view, then observes MariaDB errno 1356 when another ownerless process
   drops the base table while leaving the view definition in place, then sees
   the same view resolve again after the peer recreates the base table, with
   final view absence and recreated base-table state through ownerless/native
   reopen before and after forced `.shm` rebuild.
   Hook-build view check-option crash coverage now kills
   `CREATE VIEW ... WITH CASCADED CHECK OPTION`, `CREATE OR REPLACE VIEW ...
   WITH LOCAL CHECK OPTION`, and `ALTER VIEW ... WITH CASCADED CHECK OPTION`
   writers after native view definition storage or rewrite but before ownerless
   dictionary finish, then verifies recovered `CHECK_OPTION`, `IS_UPDATABLE`,
   view query behavior, valid through-view DML, errno 1369 for invalid DML,
   base-table writes, and ownerless/native reopen before and after forced
   `.shm` rebuild.
   Nested view check-option coverage now verifies an already-open peer observes
   an inner `CASCADED` check-option view and an outer `LOCAL` check-option view,
   inserts a row through the outer view that satisfies the outer predicate while
   violating the inner predicate, observes an outer replacement to `CASCADED`
   that rejects the same inner-predicate violation with errno 1369, observes an
   inner-view predicate alteration while the outer cascaded view remains live,
   drops both views, and verifies final view absence plus base-table durability
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Hook-build nested view check-option crash coverage kills an outer
   replacement from `LOCAL` to `CASCADED` and an inner predicate `ALTER VIEW`
   before ownerless dictionary finish, then verifies recovered inner/outer
   metadata, nested valid DML, errno 1369 for invalid inner or outer predicate
   violations, base-table writes, and ownerless/native reopen before and after
   forced `.shm` rebuild.
   View column-list coverage now verifies an already-open peer observes
   explicit column aliases created by another ownerless process, then observes
   `CREATE OR REPLACE VIEW` and `ALTER VIEW` column-list changes through
   `INFORMATION_SCHEMA.COLUMNS`, checks old exposed column names disappear and
   replacement projections are active, drops the view, and verifies final view
   absence plus base-table durability through ownerless/native reopen before
   and after forced `.shm` rebuild.
   Hook-build view column-list crash coverage now kills explicit
   column-list `CREATE VIEW`, `CREATE OR REPLACE VIEW`, and `ALTER VIEW`
   writers after native view definition storage or rewrite but before
   ownerless dictionary finish, then verifies recovered alias names, ordinal
   positions, stale-column rejection, query behavior, base-table writes, and
   ownerless/native reopen before and after forced `.shm` rebuild.
   View security/definer coverage now verifies an already-open peer observes a
   `DEFINER=CURRENT_USER SQL SECURITY DEFINER` view, then observes
   `CREATE OR REPLACE SQL SECURITY INVOKER VIEW` and
   `ALTER DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW` metadata changes
   through `INFORMATION_SCHEMA.VIEWS`, checks each changed predicate through the
   same peer handle, drops the view, and verifies final view absence plus
   base-table durability through ownerless/native reopen before and after
   forced `.shm` rebuild.
   Hook-build view security crash coverage now kills
   `CREATE DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW` and
   `CREATE OR REPLACE SQL SECURITY INVOKER VIEW` writers after native view
   definition storage, and
   `ALTER DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW` writers after native
   view definition rewrite, but before ownerless dictionary finish, then
   verifies recovered `SECURITY_TYPE`, non-empty definer metadata, view query
   behavior, base-table writes, and ownerless/native reopen before and after
   forced `.shm` rebuild.
   View idempotent DDL coverage verifies that an already-open ownerless peer
   observes `CREATE VIEW IF NOT EXISTS`, duplicate plain `CREATE VIEW` returns
   MariaDB errno 1050, repeated `CREATE VIEW IF NOT EXISTS` preserves the
   original definition, missing and repeated real `DROP VIEW IF EXISTS`
   operations refresh metadata correctly, and final view absence plus base-table
   durability survive ownerless/native reopen before and after forced `.shm`
   rebuild.
   Hook-build view idempotent crash coverage kills duplicate
   `CREATE VIEW IF NOT EXISTS` and missing `DROP VIEW IF EXISTS` no-op writers
   after MariaDB success but before ownerless dictionary finish, then verifies
   recovered original view-definition preservation, missing-view absence,
   queryability, base-table writes, and ownerless/native reopen before and after
   forced `.shm` rebuild. The two view-idempotent crash selectors are also
   registered as standalone hook CTests so CI reports their timing and failures
   separately from larger crash-tail coverage.
   Trigger
   metadata coverage now creates an InnoDB base/audit pair and an `AFTER INSERT`
   trigger from one ownerless process, verifies an already-open peer observes
   and fires the trigger, drops the trigger, and verifies later peer DML no
   longer fires it plus final trigger-file absence and base/audit durability
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Trigger variant coverage now verifies an already-open peer observes a
   `BEFORE UPDATE` trigger created by another ownerless process, fires the
   trigger's `NEW` value mutation, observes a `CREATE OR REPLACE TRIGGER`
   body change, fires an `AFTER DELETE` trigger that reads `OLD` values into an
   InnoDB audit table, drops both triggers, and verifies later peer DML no
   longer fires trigger bodies plus final trigger-file absence and base/audit
   durability through ownerless/native reopen before and after forced `.shm`
   rebuild.
   Trigger ordering coverage now verifies an already-open peer observes
   multiple `AFTER INSERT` triggers created by another ownerless process,
   checks `FOLLOWS`/`PRECEDES` `ACTION_ORDER`, runs `SHOW CREATE TRIGGER` by
   trigger name through MariaDB's trigger-name lookup path, proves firing order
   via an InnoDB audit table, drops all triggers, and verifies later peer DML
   no longer fires trigger bodies plus final trigger-file absence and
   base/audit durability through ownerless/native reopen before and after
   forced `.shm` rebuild.
   Trigger idempotent DDL coverage now verifies `CREATE TRIGGER IF NOT EXISTS`
   from another ownerless process, duplicate-create errno 1359 for the
   non-idempotent spelling, duplicate `IF NOT EXISTS` no-op preservation of the
   original trigger body, missing-trigger `DROP TRIGGER IF EXISTS` no-op
   behavior, repeated drop of the real trigger, and final trigger-file absence
   plus base/audit durability through ownerless/native reopen before and after
   forced `.shm` rebuild.
   Trigger DDL crash coverage now kills simple `CREATE TRIGGER` and
   `DROP TRIGGER` writers after MariaDB creates/removes native `.TRG` and
   `.TRN` metadata but before MyLite publishes ownerless dictionary finish;
   live-peer cleanup remains busy until no-live recovery, and recovered
   present/absent trigger metadata, file state, firing/non-firing behavior,
   ownerless reopen, forced `.shm` rebuild, and native exclusive reopen are
   verified. Replacement and ordering crash variants are covered separately,
   and idempotent no-op crash coverage now proves duplicate
   `CREATE TRIGGER IF NOT EXISTS` and missing `DROP TRIGGER IF EXISTS`
   preserve the original trigger state after a killed writer at dictionary
   finish. Delayed invalid-dependency crash coverage now proves a trigger
   whose body references a missing audit table survives the same boundary,
   reports MariaDB 1146 when fired before the dependency exists, and fires once
   the dependency is created. Explicit-definer crash coverage now proves
   `CREATE DEFINER=CURRENT_USER TRIGGER` preserves non-empty
   `INFORMATION_SCHEMA.TRIGGERS.DEFINER` metadata, `DEFINER=` in
   `SHOW CREATE TRIGGER`, and trigger firing through ownerless/native reopen
   before and after forced `.shm` rebuild. Stored-function trigger crash
   coverage now proves a trigger body that calls an exclusive-created stored
   function survives the same dictionary boundary, remains visible through
   native trigger files and `SHOW CREATE TRIGGER`, fails closed under the
   ownerless stored-routine execution guard when fired, and still fires through
   ordinary native reopen. Broader privilege/security and randomized trigger
   crash variants remain planned. The explicit-definer crash selector is also
   registered as a standalone hook CTest so CI reports its timing and failures
   separately from larger crash-tail coverage.
   Stored-routine DDL is a deliberately unsupported ownerless class for now:
   the routine path writes `mysql.proc`/`mysql.procs_priv` and a proof attempt
   hit a MariaDB error 145 `proc` system-table failure, so ownerless mode now
   rejects `CREATE`/`ALTER`/`DROP FUNCTION`, `PROCEDURE`, `PACKAGE`, and
   `PACKAGE BODY` before those uncoordinated metadata writes, including package
   specification/body rows in the stored-routine metadata path. Top-level
   ownerless routine execution is also rejected for now: top-level `CALL`
   fails at the MyLite SQL policy boundary, and `sp_head::execute_procedure()`
   plus `sp_head::execute_function()` fail while ownerless runtime hooks are
   installed so nested trigger-body procedure calls and stored-function
   expression evaluation cannot execute routine-body effects after the
   top-level statement has already passed MyLite's policy boundary. Coverage
   creates existing procedures, a stored function, and routine-calling triggers
   in exclusive mode, rejects ownerless `CALL` before its body updates an
   InnoDB table, rejects prepared ownerless `CALL` before statement allocation,
   rejects direct/prepared ownerless stored-function expressions before result
   delivery, rejects trigger-body stored procedure/function execution before
   row or audit-table mutation, and verifies routine/trigger metadata plus base
   data through ownerless/native reopen before and after forced `.shm` rebuild.
   Sequence SQL is also deliberately
   unsupported in ownerless mode: sequences are table-backed objects and
   `NEXT VALUE` / `NEXTVAL()` mutates sequence state. Ownerless mode rejects
   sequence DDL plus direct and prepared top-level value access at the MyLite
   SQL policy boundary before prepared-statement allocation, and MariaDB
   sequence value functions reject execution while ownerless runtime hooks are
   installed so hidden sequence expressions from existing metadata, including
   direct and prepared inserts through an exclusive-created `DEFAULT NEXTVAL()`
   column, cannot advance sequence state until sequence-table coordination is
   designed.
   Events and the event scheduler are likewise deliberately unsupported in
   ownerless mode: the scheduler is daemon-owned background execution, and
   event DDL/metadata statements mutate or inspect `mysql.event` outside the
   covered foreground ownerless SQL protocol. The existing MyLite
   server-surface policy rejects direct and prepared event DDL/metadata
   statements plus event scheduler variable assignments before MariaDB can
   enter the scheduler or event metadata paths. Ownerless coverage verifies the
   rejected event names remain absent from `information_schema.events` and that
   ordinary InnoDB rows survive ownerless/native reopen before and after forced
   `.shm` rebuild.
   Table-admin SQL is also deliberately unsupported in ownerless mode:
   `ANALYZE TABLE`, `CHECK TABLE`, `CHECKSUM TABLE`, `OPTIMIZE TABLE`, and
   `REPAIR TABLE` enter MariaDB SQL admin handlers that can scan table pages
   outside the proven ownerless `SELECT` snapshot-read surface, update
   statistics, check upgrade state, repair files, or rebuild tables without
   going through MyLite's ownerless dictionary DDL generation boundary.
   SQL locked-table mode is also deliberately unsupported in ownerless mode:
   `LOCK TABLES` keeps connection-level table and handler locks alive until
   `UNLOCK TABLES`, and current evidence covers primitive table-lock
   wait-entry cleanup, a hook-build negative proof that representative blocked
   `ALTER TABLE`, CHECK/FK add, `CREATE INDEX`, online/existing-index DDL,
   copy-force ALTER, charset conversion, row-format ALTER, `TRUNCATE TABLE`,
   `RENAME TABLE`, and `DROP TABLE` timeouts do not reach MyLite's local
   ownerless table-wait callback or mutate blocked metadata, plus a
   hook-build positive SQL proof that a `foreign_key_checks=0` and
   `unique_checks=0` empty-table bulk insert waiting behind a peer
   `LOCK IN SHARE MODE` reader publishes a shared external native table-wait
   registry entry and clears that entry after release, plus a hook-build crash
   proof that kills the same SQL waiter after that shared wait entry is
   published, verifies live-peer cleanup remains busy while the blocking reader
   is alive, and verifies no-live recovery removes the dead waiter before the
   interrupted insert is retried, rather than MariaDB's SQL locked-table
   lifecycle across processes.
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
   locks, and special-index recovery are designed; current ownerless policy
   rejects top-level, idempotent top-level, `ALTER TABLE`, idempotent
   `ALTER TABLE`, and inline create-time FULLTEXT/SPATIAL definitions while
   ordinary ownerless index coverage remains scoped to InnoDB secondary
   indexes.
   Partitioned table DDL is also rejected in ownerless mode until partition
   metadata, `.par` files, per-partition native engine files, partition
   maintenance, and partition-aware no-live replay are designed.
   Table `DATA DIRECTORY` and `INDEX DIRECTORY` options are rejected globally,
   including ownerless mode, because MariaDB can store native table data/index
   files or path metadata outside the MyLite database directory and MyLite has
   no external table-file lifecycle protocol. Ownerless table-directory policy
   coverage now verifies representative `CREATE TABLE`, `ALTER TABLE`, and
   partition-level `DATA DIRECTORY`/`INDEX DIRECTORY` spellings fail before
   external paths are created, with ownerless/native reopen checks before and
   after forced `.shm` rebuild.
   `ALTER TABLE ... DISCARD/IMPORT TABLESPACE` is rejected in ownerless mode
   until explicit tablespace detach/import file lifecycle metadata and recovery
   replay are designed.
   Table storage options `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`,
   `ENCRYPTED`, `ENCRYPTION_KEY_ID`, and table `TABLESPACE` are rejected in
   ownerless mode until native page-compression, encryption, and table-option
   file-layout recovery paths are designed.
   These unsupported ownerless policy gates are ordered before active-reader
   pressure throttling, so retained page-version WAL pressure does not turn
   deliberately unsupported table-admin, locked-table, flush lock/export,
   host-file export/import, tablespace detach/import, or storage-option SQL
   into a generic busy error.
   Unsafe-hook coverage kills a process
   after dictionary DDL is marked
   active but before MariaDB executes it, after successful DDL execution but
   before the ownerless dictionary generation is published stable, and after the
   generation is published stable but before the process returns. The tests
   verify recovery-sensitive active dictionary state blocks live-peer cleanup,
   no-live reopen rebuilds volatile coordination, completed DDL remains usable,
   and stable dictionary publication lets live peers proceed. Secondary-index
   crash coverage now kills standalone `CREATE INDEX` and `DROP INDEX` writers
   after native index metadata creation/removal, a
   `CREATE OR REPLACE UNIQUE INDEX` writer after native replacement metadata,
   a unique-secondary `DROP INDEX` writer after native unique-index removal,
   plus
   `ALTER TABLE ... RENAME INDEX` and
   `ALTER TABLE ... ALTER INDEX ... IGNORED`/`NOT IGNORED` writers after
   native index metadata changes but before ownerless dictionary finish, then
   verifies live-peer cleanup remains busy until no-live recovery and the
   recovered present/absent, replacement unique-key, dropped unique-key,
   renamed, and ignored/not-ignored index states remain visible through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Primary-key crash coverage now kills an
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY` writer after native
   primary-key replacement but before ownerless dictionary finish, then verifies
   recovered `PRIMARY` metadata, duplicate-key enforcement on the replacement
   key, and duplicate values allowed on the former key through ownerless/native
   reopen before and after forced `.shm` rebuild. It also kills duplicate
   `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` after MariaDB success but
   before ownerless dictionary finish, then verifies the original primary key
   remains enforced and the attempted candidate key remains non-unique through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Foreign-key crash coverage now kills an
   `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` writer after native
   foreign-key metadata creation but before ownerless dictionary finish, then
   verifies recovered FK metadata, orphan-row rejection, valid child writes, and
   ownerless/native reopen before and after forced `.shm` rebuild.
   Foreign-key DROP crash coverage now kills an
   `ALTER TABLE ... DROP FOREIGN KEY` writer after native foreign-key metadata
   removal but before ownerless dictionary finish, then verifies recovered FK
   metadata absence, orphan-row writes, parent deletes, and ownerless/native
   reopen before and after forced `.shm` rebuild.
   CHECK constraint crash coverage now kills an
   `ALTER TABLE ... ADD CONSTRAINT ... CHECK` writer after native
   table-definition mutation but before ownerless dictionary finish, then
   verifies recovered table-level CHECK metadata, errno 4025 enforcement, valid
   post-recovery writes, and ownerless/native reopen before and after forced
   `.shm` rebuild.
   CHECK constraint DROP crash coverage now kills an
   `ALTER TABLE ... DROP CONSTRAINT` writer after native CHECK metadata removal
   but before ownerless dictionary finish, then verifies recovered CHECK
   metadata absence, formerly invalid writes, and ownerless/native reopen before
   and after forced `.shm` rebuild.
   View crash coverage now kills simple `CREATE VIEW` and `DROP VIEW` writers
   after native view definition-file creation/removal but before ownerless
   dictionary finish, then verifies recovered present/absent view metadata,
   `.frm` file state, view query behavior, base-table writes, and
   ownerless/native reopen before and after forced `.shm` rebuild.
   Hook-build view replacement crash coverage now also kills
   `CREATE OR REPLACE VIEW` and `ALTER VIEW` writers after native view
   definition rewrite but before ownerless dictionary finish, then verifies
   recovered replacement/altered metadata and query behavior through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Hook-build view column-list crash coverage kills explicit column-list
   create, replace, and alter writers after native view definition storage or
   rewrite but before ownerless dictionary finish, then verifies recovered
   alias metadata and query behavior through ownerless/native reopen before and
   after forced `.shm` rebuild.
   Hook-build view check-option crash coverage kills cascaded create, local
   replacement, and cascaded alter writers after native view definition storage
   or rewrite but before ownerless dictionary finish, then verifies recovered
   check-option metadata and DML enforcement through ownerless/native reopen
   before and after forced `.shm` rebuild. Hook-build nested view check-option
   crash coverage kills outer replacement and inner alter writers before
   ownerless dictionary finish, then verifies recovered nested metadata and
   DML enforcement through ownerless/native reopen before and after forced
   `.shm` rebuild.
   Hook-build view security crash coverage kills explicit definer create,
   invoker replacement, and definer-security alter writers after native view
   definition storage or rewrite but before ownerless dictionary finish, then
   verifies recovered security metadata and query behavior through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Trigger crash coverage now kills simple `CREATE TRIGGER` and `DROP TRIGGER`
   writers after native `.TRG`/`.TRN` metadata creation/removal, plus
   duplicate `CREATE TRIGGER IF NOT EXISTS` and missing
   `DROP TRIGGER IF EXISTS` no-op writers, but before ownerless dictionary
   finish, then verifies recovered present/absent trigger metadata, native
   trigger-file state, trigger firing/non-firing or no-op preservation
   behavior, `SHOW CREATE TRIGGER` rejection for the dropped trigger, and
   ownerless/native reopen before and after forced `.shm` rebuild.
   Column-add/drop/modify/rename crash coverage now kills
   `ALTER TABLE ... ADD COLUMN`, `ALTER TABLE ... DROP COLUMN`,
   `ALTER TABLE ... MODIFY COLUMN`, and `ALTER TABLE ... RENAME COLUMN` after
   native table-definition mutation but before ownerless dictionary finish, then
   verifies recovered added-column metadata/default values, absent
   dropped-column metadata, modified-column width/default metadata, renamed-column
   metadata with old-name rejection and new-name writes, generated-column and
   CHECK expression behavior after a dependent column rename, existing-row
   values, widened-value writes, and later inserts through ownerless/native
   reopen before and after forced `.shm` rebuild. Force-rebuild crash coverage
   now kills an `ALTER TABLE ... FORCE, ALGORITHM=COPY` writer after native
   table-copy rebuild but before ownerless dictionary finish, then verifies
   recovered InnoDB table/space/index metadata, copied payload bytes, and later
   writes through ownerless/native reopen before and after forced `.shm` rebuild.
   Row-format crash coverage now kills an
   `ALTER TABLE ... ROW_FORMAT=DYNAMIC` writer after native row-format rebuild
   but before ownerless dictionary finish, then verifies recovered dynamic
   row-format metadata, retained row payloads, and later writes through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Compressed row-format crash coverage now kills
   `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`,
   `KEY_BLOCK_SIZE=2`, `KEY_BLOCK_SIZE=4`, `KEY_BLOCK_SIZE=8`, and
   `KEY_BLOCK_SIZE=16` writers after
   native compressed table-option rebuild but before ownerless dictionary
   finish, then verifies recovered compressed metadata, retained prepared BLOB
   payloads, native ZBLOB page evidence, and later writes through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Ownerless online
   DDL option coverage now proves already-open peers refresh after accepted
   explicit `ALGORITHM=NOCOPY, LOCK=NONE` secondary-index creation,
   `ALGORITHM=NOCOPY, LOCK=NONE` secondary-index drop,
   `ALGORITHM=NOCOPY, LOCK=DEFAULT` secondary-index creation,
   `ALGORITHM=NOCOPY, LOCK=DEFAULT` secondary-index drop,
   `ALGORITHM=NOCOPY, LOCK=SHARED` secondary-index creation and drop,
   `ALGORITHM=NOCOPY, LOCK=EXCLUSIVE` secondary-index creation and drop,
   `ALGORITHM=INPLACE, LOCK=NONE` secondary-index creation, standalone drop,
   and re-add,
   `ALGORITHM=INPLACE, LOCK=SHARED` secondary-index creation, standalone drop,
   and re-add,
   `ALGORITHM=INPLACE, LOCK=DEFAULT` secondary-index creation,
   `ALGORITHM=INPLACE, LOCK=DEFAULT` secondary-index drop,
   `ALGORITHM=INPLACE, LOCK=EXCLUSIVE` secondary-index creation and drop,
   `ALGORITHM=INPLACE, LOCK=SHARED` unique secondary-index creation and drop,
   explicit no-lock index ignored/not-ignored toggles, instant
   `ALGORITHM=INSTANT, LOCK=DEFAULT` column add/drop, and
   `ALGORITHM=COPY, LOCK=EXCLUSIVE` column/rebuild paths. Broader online DDL
   classes and option combinations beyond the covered ordinary/unique index,
   accepted explicit online DDL option variants, secondary-index rename,
   idempotent standalone index create/drop, ignored-index metadata,
   primary-key replacement, foreign-key ALTER,
   same-schema foreign-key parent-table/child-table rename, cross-schema
   foreign-key parent-table/child-table rename, same-schema foreign-key
   multi-pair parent/child rename, cross-schema foreign-key multi-pair
   parent/child rename, CHECK constraint ALTER, generated-column ALTER including
   same-kind expression replacement, table charset conversion, row-format rebuild,
   table comment metadata,
   `ALTER TABLE ... FORCE` rebuild, column-default SET/DROP, column
   idempotent ADD/DROP, column-shape,
   explicit instant ADD/DROP/reorder, instant FIRST/AFTER stored-column
   placement including `LOCK=DEFAULT`, `LOCK=SHARED`, and `LOCK=EXCLUSIVE`
   placement variants, instant column rename including a `LOCK=DEFAULT` rename
   variant, and instant virtual generated-column add/drop including
   `LOCK=SHARED`/`LOCK=EXCLUSIVE` variants remain covered.
   Broader instant variants and broader online DDL option combinations outside
   the covered `LOCK=DEFAULT` instant add/drop, stored-column placement,
   `LOCK=SHARED`/`LOCK=EXCLUSIVE` stored-column placement,
   `LOCK=SHARED`/`LOCK=EXCLUSIVE` virtual generated-column add/drop, rename,
   `NOCOPY`
   secondary-index add/drop with `LOCK=NONE`, `LOCK=DEFAULT`, `LOCK=SHARED`,
   or `LOCK=EXCLUSIVE`, and `INPLACE` secondary-index add/drop with
   `LOCK=NONE`, `LOCK=SHARED`, `LOCK=DEFAULT`, or `LOCK=EXCLUSIVE` shapes and
   representative `COPY` rebuilds with `LOCK=EXCLUSIVE`; external randomized
   DDL oracles remain planned.
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
   Stale-reader rebuild coverage now leaves retained page-version WAL records
   for updated file-per-table objects while an old snapshot pin is live, then
   kills the pin owner and verifies the no-live rebuild checkpoints those
   reader-boundary records before SQL execution when no native writer recovery
   evidence remains. The focused cases preserve a dropped table's final absent
   state, a renamed table's moved schema/name and `.frm`/`.ibd` files, a
   rename-away plus new original-name `CREATE TABLE` pair's distinct final
   rows, indexes, files, and page-0 space identities, a truncated table's
   post-truncate rows and file paths, and a dropped schema's absent
   schema/table metadata plus removed directory and table files for multiple
   schema-owned InnoDB tables. Focused
   secondary-index crash coverage preserves completed standalone `CREATE INDEX`
   and `DROP INDEX` boundaries before ownerless dictionary finish, and focused
   unique replacement crash coverage preserves completed
   `CREATE OR REPLACE UNIQUE INDEX` metadata/enforcement movement, then verifies
   recovered present-index metadata and forced-index reads, absent-index
   metadata and forced-index rejection, and replacement unique-key enforcement;
   focused unique drop crash coverage preserves completed native unique-index
   removal, then verifies absent metadata, forced-index rejection, formerly
   duplicate writes allowed after recovery, and final rows through
   ownerless/native reopen before and after forced `.shm` rebuild.
   Focused column-add/drop/modify/rename
   crash coverage preserves completed `ALTER TABLE ... ADD COLUMN`,
   `ALTER TABLE ... DROP COLUMN`, `ALTER TABLE ... MODIFY COLUMN`, and
   `ALTER TABLE ... RENAME COLUMN` boundaries before ownerless dictionary
   finish, then verifies recovered added-column/default metadata, absent
   dropped-column metadata, modified-column width/default metadata,
   renamed-column metadata, dependent generated-column and CHECK expression
   behavior, and row values through ownerless/native reopen before and after
   forced `.shm` rebuild.
   Focused column-idempotent crash coverage preserves completed no-op
   `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and
   `ALTER TABLE ... DROP COLUMN IF EXISTS` dictionary boundaries, then verifies
   the duplicate-add path keeps the original column default, the missing-drop
   path keeps the real column and missing name absent, plain non-idempotent
   retries continue returning MariaDB 1060/1091, and post-recovery writes work
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Focused column missing-`IF EXISTS` crash coverage preserves completed no-op
   `ALTER TABLE ... MODIFY COLUMN IF EXISTS` and
   `ALTER TABLE ... RENAME COLUMN IF EXISTS` dictionary boundaries, then
   verifies the real column's metadata/default, missing and attempted renamed
   column absence, MariaDB 1054 plain retry errno, and post-recovery writes
   through ownerless/native reopen before and after forced `.shm` rebuild.
   Focused generated-column/CHECK expression tables extend the missing rename,
   change, and default no-op cases by verifying the real column, stored and
   virtual generated expression values, real defaults, CHECK enforcement, and
   missing attempted names survive the same recovery boundary.
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
   coverage now starts a live temporary-table peer before starting the next
   temporary-table peer so the focused cases isolate temporary tablespace
   lifecycle from generic ownerless startup-storm coverage. It then verifies
   two ownerless peers can each hold a same-named InnoDB temporary table, each
   peer's rows remain connection-local while another ownerless handle operates,
   one temporary-table peer can be killed while another remains live, a new
   ownerless opener can still use its own same-named temporary table, and the
   name can be reused for a persistent InnoDB table after the temporary
   sessions are gone. Opt-in stress coverage now churns same-named
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
   peer-refresh coverage verifies foreign-key cascade behavior including a
   multi-hop cascade chain, CHECK
   constraint add/drop enforcement, generated-column recalculation,
   generated-column ALTER add/drop and same-kind expression replacement with
   stored and virtual generated expressions, standalone stored and virtual
   generated-column secondary-index
   create/use/drop including prefix key parts with recalculation after peer DML,
   indexed generated-column expression replacement,
   indexed generated-column expression duplicate-key policy,
   generated-column primary-key rejection policy, generated-column
   nondeterministic-expression policy,
   `CREATE TABLE ... LIKE`, `CREATE TABLE ... SELECT`,
   table idempotent `CREATE TABLE IF NOT EXISTS`, `CREATE OR REPLACE TABLE`,
   and `DROP TABLE IF EXISTS`,
   and an online/in-place index alter plus column add/modify/rename/drop ALTERs,
   column idempotent `ADD COLUMN IF NOT EXISTS` and `DROP COLUMN IF EXISTS`,
   standalone idempotent `CREATE INDEX IF NOT EXISTS` and
   `DROP INDEX IF EXISTS`,
   explicit instant ADD/DROP/reorder column metadata, and instant-column
   variant metadata for FIRST/AFTER stored placement, column rename, and virtual
   generated-column add/drop performed by another ownerless process. Hook-build
   crash coverage now also kills
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY` before ownerless
   dictionary finish and verifies recovered replacement primary-key metadata,
   duplicate enforcement, and former-key duplicate allowance, and kills
   duplicate `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` before ownerless
   dictionary finish while verifying original-key preservation and candidate-key
   non-uniqueness through ownerless and native reopen. Hook-build crash coverage
   also kills
   `ALTER TABLE ... ADD COLUMN`,
   `ALTER TABLE ... DROP COLUMN`, `ALTER TABLE ... MODIFY COLUMN`, and
   `ALTER TABLE ... RENAME COLUMN` writers before ownerless dictionary finish
   and verifies recovered column metadata/defaults, absent dropped-column
   metadata, modified-column width/default metadata, renamed-column metadata,
   and dependent generated-column/CHECK expression behavior through ownerless
   and native reopen. Hook-build crash coverage also kills
   duplicate `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing
   `ALTER TABLE ... DROP COLUMN IF EXISTS` no-op writers before ownerless
   dictionary finish and verifies preserved column metadata/defaults,
   missing-column absence, MariaDB 1060/1091 retry errno, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild. Hook-build crash coverage
   also kills
   missing `ALTER TABLE ... MODIFY COLUMN IF EXISTS` and
   `ALTER TABLE ... RENAME COLUMN IF EXISTS` no-op writers before ownerless
   dictionary finish and verifies preserved real-column metadata/defaults,
   missing and attempted renamed-column absence, MariaDB 1054 retry errno,
   post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild.
   The missing rename, change, and default branches also have
   generated-column/CHECK expression-table crash variants that prove generated
   values, real defaults, and CHECK enforcement are preserved across ownerless
   and native reopen.
   Hook-build crash coverage also kills
   `ALTER TABLE ... FORCE, ALGORITHM=COPY` before ownerless dictionary finish
   and verifies recovered InnoDB table/space/index metadata, copied payloads,
   and post-recovery writes through ownerless and native reopen. Hook-build
   crash coverage also kills `ALTER TABLE ... ROW_FORMAT=DYNAMIC` before
   ownerless dictionary finish and verifies recovered native dynamic row-format
   metadata, retained rows, and post-recovery writes through ownerless and
   native reopen. Hook-build crash coverage also kills
   `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` before ownerless dictionary
   finish and verifies recovered FK metadata, orphan-row rejection without a
   pending rejected child row surviving to `COMMIT`, and valid child writes
   through ownerless and native reopen. Hook-build crash coverage
   also kills
   `ALTER TABLE ... DROP FOREIGN KEY` before ownerless dictionary finish and
   verifies recovered FK metadata absence plus post-drop orphan child writes and
   parent deletes through ownerless and native reopen. Hook-build crash coverage
   also kills `ALTER TABLE ... ADD CONSTRAINT ... CHECK` before ownerless
   dictionary finish and verifies recovered CHECK metadata plus errno 4025
   enforcement through ownerless and native reopen. Hook-build crash coverage
   also kills `ALTER TABLE ... DROP CONSTRAINT` for CHECK constraints before
   ownerless dictionary finish and verifies recovered CHECK metadata absence
   plus formerly invalid writes through ownerless and native reopen. Hook-build
   crash coverage also kills representative `CREATE TABLE ... LIKE` and
   `CREATE TABLE ... SELECT` writers after native destination table creation
   but before ownerless dictionary finish. The LIKE and focused
   no-definition-list CTAS selectors prove live-peer dictionary cleanup after
   native `FILE_CREATE`; coverage verifies recovered `.frm`/`.ibd` files,
   destination table/column metadata, copied secondary-index metadata for
   `LIKE`, CTAS copied rows, post-recovery writes, ownerless/native reopen, and
   forced `.shm` rebuild. Hook-build
   crash coverage also kills a representative `CREATE OR REPLACE TABLE` writer
   after native old-table replacement but before ownerless dictionary finish
   and verifies recovered replacement `.frm`/`.ibd` files, old-column/index
   absence, new-column/index metadata, empty replacement rowset,
   post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild.
   Hook-build after-drop crash coverage also kills a representative
   `CREATE OR REPLACE TABLE` writer between MariaDB's old-table removal and
   replacement creation, verifies the table remains absent through
   ownerless/native reopen and forced `.shm` rebuild, and verifies same-name
   recreation after recovery.
   Hook-build crash coverage also kills representative
   `CREATE OR REPLACE TABLE ... LIKE` and
   `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy writers before
   ownerless dictionary finish and verifies recovered replacement files,
   old-column/index absence, copied `LIKE` secondary-index metadata, CTAS
   copied rows, post-recovery writes, ownerless/native reopen, and forced
   `.shm` rebuild.
   Hook-build crash coverage also kills duplicate
   `CREATE TABLE IF NOT EXISTS` and missing `DROP TABLE IF EXISTS` no-op
   writers before ownerless dictionary finish and verifies preserved native
   table metadata, missing-table absence, ownerless/native reopen, and forced
   `.shm` rebuild.
   Hook-build
   crash coverage also kills simple `CREATE VIEW`, `DROP VIEW`,
   `CREATE OR REPLACE VIEW`, and `ALTER VIEW` before ownerless dictionary
   finish, plus explicit column-list create/replace/alter, check-option
   create/replacement/alter, nested check-option outer replacement/inner alter,
   explicit definer create, and invoker replacement view
   writers, and verifies recovered present/absent, rewritten, column-list,
   check-option, or security view metadata,
   `.frm` file state, view query behavior, and base-table writes through
   ownerless and native reopen. Hook-build
   crash coverage also kills simple `CREATE TRIGGER`, `DROP TRIGGER`,
   duplicate `CREATE TRIGGER IF NOT EXISTS`, and missing
   `DROP TRIGGER IF EXISTS` before ownerless dictionary finish and verifies
   recovered present/absent trigger metadata, `.TRG`/`.TRN` file state,
   trigger firing/non-firing or no-op preservation behavior, and
   `SHOW CREATE TRIGGER` rejection for the dropped trigger through ownerless
   and native reopen. Hook-build
   crash coverage also kills
   `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`,
   `KEY_BLOCK_SIZE=2`, `KEY_BLOCK_SIZE=4`, `KEY_BLOCK_SIZE=8`, and
   `KEY_BLOCK_SIZE=16` before ownerless
   dictionary finish and verifies recovered native compressed metadata,
   retained prepared BLOB rows, ZBLOB page evidence, and post-recovery writes
   through ownerless and native reopen. The broader
   DDL and instant-variant selectors also verify that the final altered,
   copy, and instant table state survives no-live ownerless and native exclusive
   reopen before and after forced `.shm` rebuild. Schema lifecycle
   coverage adds ownerless `CREATE DATABASE` plus InnoDB table creation,
   peer-write visibility, `DROP DATABASE`, and absent-schema reopen checks.
   Stale-reader created-table replay adds no-live rebuild coverage for a
   file-per-table InnoDB table created while an older repeatable-read snapshot
   pins retained page-version WAL.
   Stale-reader same-name recreate replay adds no-live rebuild coverage for a
   dropped and recreated InnoDB table whose final metadata shape and native
   page-0 space id differ from the dropped table.
   Stale-reader rename-create replay adds no-live rebuild coverage for an
   updated InnoDB table renamed away while a different new InnoDB table reuses
   the original SQL name, preserving both final tables and their distinct
   page-0 space ids through ownerless/native reopen.
   Stale-reader schema-drop replay adds no-live rebuild coverage for retained
   page-version WAL from multiple tables inside a dropped schema.
   Schema default DDL coverage adds ownerless `CREATE DATABASE ... DEFAULT
   CHARACTER SET/COLLATE`, peer-visible schema defaults and native `db.opt`
   presence, ownerless `ALTER DATABASE ... DEFAULT CHARACTER SET/COLLATE`,
   inherited column collation checks for a later table, `DROP DATABASE`, and
   absent-schema reopen checks before and after forced `.shm` rebuild.
   Schema idempotent DDL coverage adds `CREATE SCHEMA IF NOT EXISTS`,
   duplicate `CREATE DATABASE IF NOT EXISTS` default-preservation checks,
   `DROP SCHEMA IF EXISTS` for absent and existing schemas, and absent-schema
   reopen checks before and after forced `.shm` rebuild.
   Hook-build schema-idempotent crash coverage adds killed duplicate
   `CREATE DATABASE IF NOT EXISTS` and missing `DROP SCHEMA IF EXISTS` no-op
   writers after MariaDB returns success but before ownerless dictionary finish,
   with preserved original schema defaults, preserved real schema/table state,
   missing-schema absence, ownerless/native reopen, and forced `.shm` rebuild
   checks.
   Hook-build schema-create crash coverage adds a killed
   `CREATE DATABASE ... DEFAULT CHARACTER SET/COLLATE` writer after native
   schema directory/`db.opt` creation but before ownerless dictionary finish,
   with recovered default metadata, post-recovery InnoDB table writes,
   ownerless/native reopen, and forced `.shm` rebuild checks.
   Hook-build schema-alter crash coverage adds a killed
   `ALTER DATABASE ... DEFAULT CHARACTER SET/COLLATE` writer after native
   `db.opt` rewrite but before ownerless dictionary finish, with recovered
   default metadata, pre-alter table collation preservation, post-recovery
   default inheritance, ownerless/native reopen, and forced `.shm` rebuild
   checks.
   Cross-schema rename coverage adds ownerless `RENAME TABLE app.t TO other.t`,
   already-open peer metadata refresh for the old and new schema-qualified
   names, peer writes through the moved table, `.frm`/`.ibd` movement checks,
   and ownerless/native reopen checks before and after forced `.shm` rebuild.
   Multi-rename-cycle coverage adds a three-pair
   `RENAME TABLE left TO tmp, right TO left, tmp TO right` swap, already-open
   peer metadata refresh for the final names, InnoDB `SPACE` identity swap
   checks, peer writes through both swapped tables, and ownerless/native reopen
   checks before and after forced `.shm` rebuild. View DDL variant coverage adds
   ownerless `CREATE OR REPLACE VIEW` and `ALTER VIEW` definition replacement
   over an InnoDB base table, proving an already-open peer refreshes changed
   view columns and predicates before final drop and ownerless/native reopen.
   View column-list coverage adds explicit view column aliases across create,
   replacement, and alter definitions, proving already-open peers refresh
   `INFORMATION_SCHEMA.COLUMNS` metadata and exposed column names before final
   absent-view reopen checks before and after forced `.shm` rebuild.
   View check-option coverage adds ownerless updatable view
   `WITH LOCAL/CASCADED CHECK OPTION` metadata refresh, valid insert/update
   through the view, invalid insert/update MariaDB errno 1369 checks, and final
   absent-view reopen checks before and after forced `.shm` rebuild.
   Prepared view DML coverage adds prepared `SELECT`, `INSERT`, and `UPDATE`
   through an ownerless check-option view, including invalid prepared
   insert/update errno 1369 checks and reuse after a peer replaces the view
   predicate.
   Non-updatable view diagnostics coverage adds aggregate-view
   `IS_UPDATABLE = 'NO'` refresh, direct `INSERT` errno 1471, direct
   `UPDATE`/`DELETE` errno 1288, rejected `WITH CHECK OPTION` errno 1368, and
   failed-write immutability checks.
   Prepared non-updatable diagnostics add ownerless prepared-DML step-time
   `INSERT` errno 1471 and `UPDATE`/`DELETE` errno 1288 for the original and
   peer-replaced aggregate view definitions.
   Invalid view dependency diagnostics add ownerless view query errno 1356 after
   peer base-table drop, then recovery of the same view after peer base-table
   recreation.
   Nested view check-option coverage adds ownerless inner/outer updatable views
   that distinguish outer `LOCAL` from outer `CASCADED` propagation, refresh an
   altered inner predicate under an already-open outer cascaded view, and verify
   final absent-view reopen checks before and after forced `.shm` rebuild.
   View security/definer coverage adds ownerless
   `DEFINER=CURRENT_USER SQL SECURITY DEFINER`, replacement to
   `SQL SECURITY INVOKER`, and alteration back to `SQL SECURITY DEFINER`
   metadata refresh, with final absent-view reopen checks before and after
   forced `.shm` rebuild.
   View idempotent DDL coverage adds ownerless `CREATE VIEW IF NOT EXISTS`,
   duplicate-create errno 1050, no-op duplicate definition preservation,
   missing-view `DROP VIEW IF EXISTS`, repeated real `DROP VIEW IF EXISTS`, and
   final absent-view reopen checks before and after forced `.shm` rebuild.
   View metadata coverage adds ownerless `CREATE VIEW` over an InnoDB base
   table, peer-visible view queries, `DROP VIEW`, and absent-view reopen checks
   before and after forced `.shm` rebuild. Hook-build crash coverage also
   preserves completed simple view create/drop boundaries, completed
   `CREATE OR REPLACE VIEW` and `ALTER VIEW` rewrites, plus duplicate
   `CREATE VIEW IF NOT EXISTS` and missing `DROP VIEW IF EXISTS` no-op
   boundaries, plus explicit column-list create/replace/alter, check-option
   create/replacement/alter, nested check-option outer replacement/inner alter,
   explicit definer create, and invoker replacement
   boundaries before ownerless dictionary finish, then verifies present/absent,
   rewritten, column-list, check-option, or security view metadata, preserved
   original or replacement view
   definitions, query behavior, and base-table writes through ownerless/native
   reopen before and after forced `.shm` rebuild. Trigger metadata coverage adds
   ownerless `CREATE TRIGGER` over an InnoDB base table, peer-fired audit-table
   effects, `DROP TRIGGER`, and absent-trigger reopen checks before and after
   forced `.shm` rebuild. Trigger variant coverage adds ownerless
   `CREATE OR REPLACE TRIGGER`, `BEFORE UPDATE` `NEW` mutation, and
   `AFTER DELETE` `OLD` audit effects, proving an already-open peer refreshes
   changed trigger bodies before final drop and ownerless/native reopen.
   Trigger ordering coverage adds ownerless `FOLLOWS`/`PRECEDES`
   `ACTION_ORDER`, firing-order checks, and `SHOW CREATE TRIGGER` lookup by
   trigger name, proving an already-open peer refreshes multiple trigger
   definitions stored for one base table before final drop and ownerless/native
   reopen.
   Trigger idempotent DDL coverage adds ownerless
   `CREATE TRIGGER IF NOT EXISTS`, duplicate-create errno 1359, no-op duplicate
   preservation, missing-trigger `DROP TRIGGER IF EXISTS`, and repeated
   real-trigger drop checks before final ownerless/native reopen.
   Standalone index DDL coverage adds ownerless
   `CREATE INDEX`/`DROP INDEX` over an InnoDB base table, already-open peer
   metadata refresh through `information_schema.statistics`, forced-index use
   before drop, and final absent-index checks before and after forced `.shm`
   rebuild. Hook-build crash coverage now also kills standalone
   `CREATE INDEX` and `DROP INDEX` writers before ownerless dictionary finish
   and verifies recovered present-index metadata/use plus absent-index
   metadata/rejection through ownerless and native reopen. Hook-build crash
   coverage also kills `CREATE OR REPLACE UNIQUE INDEX` after native
   replacement metadata before ownerless dictionary finish and verifies recovered
   replacement-key metadata plus duplicate-key enforcement, and kills
   unique-secondary `DROP INDEX` after native unique-index removal before
   ownerless dictionary finish and verifies absent metadata plus formerly
   duplicate writes accepted after recovery. Standalone index
   idempotent DDL coverage adds ownerless
   `CREATE INDEX IF NOT EXISTS`, duplicate-create errno 1061, duplicate no-op
   preservation of the original indexed column, `CREATE OR REPLACE INDEX`
   replacement of the existing index name over another key part, missing-index
   `DROP INDEX IF EXISTS`, repeated real-index drop checks, plus matching
   `ALTER TABLE ... ADD INDEX IF NOT EXISTS` and
   `ALTER TABLE ... DROP INDEX IF EXISTS` table-element create/no-op/drop
   checks before final ownerless/native reopen. It also covers ordinary inline
   `CREATE TABLE ... INDEX` creation on a new table and MariaDB's duplicate
   inline key-name errno 1061 behavior, with no failed-table leak.
   Hook-build index-idempotent crash coverage adds killed duplicate top-level
   `CREATE INDEX IF NOT EXISTS`, missing top-level `DROP INDEX IF EXISTS`,
   duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS`, and missing
   `ALTER TABLE ... DROP INDEX IF EXISTS` no-op writers after MariaDB returns
   success but before ownerless dictionary finish, with preserved original key
   part, missing-index absence, post-recovery writes, ownerless/native reopen,
   and forced `.shm` rebuild checks.
   Unique-index idempotent DDL
   coverage adds top-level `CREATE UNIQUE INDEX IF NOT EXISTS`, duplicate
   plain-create errno 1061, duplicate no-op preservation of the original unique
   key, `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` create/no-op checks,
   duplicate plain ALTER-add errno 1061, and missing/repeated ALTER-drop checks
   before final ownerless/native reopen. Hook-build unique-index idempotent
   crash coverage kills duplicate top-level
   `CREATE UNIQUE INDEX IF NOT EXISTS` and duplicate
   `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op writers after MariaDB
   returns success but before ownerless dictionary finish, with preserved unique
   key parts, duplicate-key enforcement, attempted-key non-enforcement,
   ownerless/native reopen, and forced `.shm` rebuild checks.
   Secondary-index rename coverage adds ownerless
   `ALTER TABLE ... RENAME INDEX`, already-open peer metadata refresh for the
   old and new index names, forced-index rejection for the old name, forced-index
   use for the new name, and final renamed-index checks before and after forced
   `.shm` rebuild. Hook-build crash coverage also kills a rename-index writer
   before ownerless dictionary finish and verifies recovered old-name absence,
   new-index metadata/use, later DML, ownerless/native reopen, and forced
   `.shm` rebuild. Descending-index coverage adds standalone ownerless
   `CREATE INDEX ... (value DESC)` and `DROP INDEX`, peer-visible
   `information_schema.statistics.COLLATION = 'D'`, forced-index use while the
   index exists, and final absent-index checks before and after forced `.shm`
   rebuild. Mixed-direction index coverage adds standalone ownerless
   `CREATE INDEX ... (tenant_id ASC, score DESC)` and `DROP INDEX`,
   peer-visible `information_schema.statistics.COLLATION = 'A'` for sequence 1
   and `COLLATION = 'D'` for sequence 2, forced-index use while the index
   exists, and final absent-index checks before and after forced `.shm` rebuild.
   Prefix-direction index coverage adds standalone ownerless
   `CREATE INDEX ... (code(4) DESC, score ASC)`, peer-visible `SUB_PART = 4`
   plus `COLLATION = 'D'` for the first key part and `COLLATION = 'A'` for the
   second key part, forced-index use while the index exists, and final
   absent-index checks before and after forced `.shm` rebuild.
   Unique prefix-direction index coverage adds standalone ownerless
   `CREATE UNIQUE INDEX ... (code(4) DESC, score ASC)`, peer-visible
   `NON_UNIQUE = 0`, `SUB_PART = 4`, and key-part `COLLATION` metadata,
   duplicate prefix-plus-score rejection before drop, duplicate insertion after
   drop, and final absent-index checks before and after forced `.shm` rebuild.
   Prefix-index coverage adds standalone ownerless
   `CREATE INDEX ... (code(4))` and `DROP INDEX`, peer-visible
   `information_schema.statistics.SUB_PART = 4`, forced-index use while the
   index exists, and final absent-index checks before and after forced `.shm`
   rebuild. TEXT/BLOB prefix-index coverage adds standalone ownerless
   `CREATE INDEX ... (body(5))` and `CREATE INDEX ... (payload(4))`,
   peer-visible `SUB_PART = 5` and `SUB_PART = 4`, forced-index use while the
   indexes exist, writes through both native prefix indexes, and final
   absent-index checks before and after forced `.shm` rebuild. TEXT/BLOB
   prefix-direction index coverage adds standalone ownerless
   `CREATE INDEX ... (body(5) DESC)` and
   `CREATE INDEX ... (payload(4) DESC)`, peer-visible `SUB_PART = 5` and
   `SUB_PART = 4` plus `COLLATION = 'D'`, forced-index use while both indexes
   exist, writes through both native prefix-direction indexes, and final
   absent-index checks before and after forced `.shm` rebuild. Unique
   TEXT/BLOB prefix-index coverage adds standalone ownerless
   `CREATE UNIQUE INDEX ... (body(5))` and
   `CREATE UNIQUE INDEX ... (payload(4))`, peer-visible `NON_UNIQUE = 0` plus
   `SUB_PART = 5` and `SUB_PART = 4`, duplicate TEXT-prefix and BLOB-prefix
   rejection before drop, formerly duplicate insertion after drop, and final
   absent-index checks before and after forced `.shm` rebuild. Unique
   TEXT/BLOB prefix-direction index coverage adds standalone ownerless
   `CREATE UNIQUE INDEX ... (body(5) DESC)` and
   `CREATE UNIQUE INDEX ... (payload(4) DESC)`, peer-visible
   `NON_UNIQUE = 0`, `SUB_PART = 5` and `SUB_PART = 4` plus
   `COLLATION = 'D'`, duplicate TEXT-prefix and BLOB-prefix rejection before
   drop, formerly duplicate insertion after drop, and final absent-index checks
   before and after forced `.shm` rebuild. utf8mb4 unique prefix-index
   coverage adds standalone ownerless
   `CREATE UNIQUE INDEX ... (code(1))` over a `utf8mb4` string whose first
   character is four bytes, peer-visible `NON_UNIQUE = 0` plus
   `SUB_PART = 1`, forced-index use, duplicate first-character prefix
   rejection before drop, formerly duplicate insertion after drop, and final
   absent-index checks before and after forced `.shm` rebuild. Unique
   prefix-index coverage adds standalone ownerless
   `CREATE UNIQUE INDEX ... (code(4))`, peer-visible `NON_UNIQUE = 0` plus
   `SUB_PART = 4`, duplicate-prefix enforcement before drop, duplicate-prefix
   insertion after drop, and final absent-index checks before and after forced
   `.shm` rebuild. Ignored-index coverage adds ownerless
   `ALTER TABLE ... ALTER INDEX ... IGNORED` and `NOT IGNORED`, already-open
   peer metadata refresh through `information_schema.statistics.IGNORED`, DML
   while the index is ignored, forced-index use after the index is restored, and
   final not-ignored index checks before and after forced `.shm` rebuild.
   Hook-build crash coverage also kills ignored and not-ignored writers before
   ownerless dictionary finish and verifies recovered `IGNORED = 'YES'`,
   recovered final `IGNORED = 'NO'`, final forced-index reads, ownerless/native
   reopen, and forced `.shm` rebuild.
   Unique-index coverage adds multi-column `CREATE UNIQUE INDEX`,
   peer-visible `NON_UNIQUE = 0` metadata, duplicate-key enforcement before
   drop, duplicate-key insertion after drop, and final absent-index checks
   before and after forced `.shm` rebuild. Unique descending-index coverage
   adds `CREATE UNIQUE INDEX ... (tenant_id, score DESC)`, peer-visible
   `NON_UNIQUE = 0` plus `COLLATION = 'D'` metadata, duplicate-key
   enforcement before drop, duplicate-key insertion after drop, and final
   absent-index checks before and after forced `.shm` rebuild. Primary-key
   coverage adds initial peer-visible `PRIMARY(id)` metadata, duplicate plain
   primary-key add errno 1068,
   `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS (code)` no-op preservation,
   then
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)`, peer-visible
   `PRIMARY` metadata on the replacement column, duplicate-key enforcement on
   the new key, old-key duplicate insertion after replacement, and final
   replacement-primary-key checks before and after forced `.shm` rebuild.
   Hook-build crash coverage also kills duplicate
   `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` before ownerless dictionary
   finish and verifies the recovered no-op preserves `PRIMARY(id)` while `code`
   remains non-unique through ownerless/native reopen before and after forced
   `.shm` rebuild. A
   descending primary-key replacement variant adds
   `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code DESC)`,
   peer-visible `PRIMARY` metadata with `COLLATION = 'D'`, duplicate-key
   enforcement on the replacement key, old-key duplicate insertion after
   replacement, and final replacement-primary-key checks before and after
   forced `.shm` rebuild. A composite direction primary-key replacement variant
   adds `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (tenant_id ASC, code DESC)`,
   peer-visible `PRIMARY` metadata with `COLLATION = 'A'`/`'D'`, duplicate-key
   enforcement on the composite replacement key, old-key duplicate insertion
   after replacement, and final replacement-primary-key checks before and after
   forced `.shm` rebuild. An
   AUTO_INCREMENT primary-key replacement variant keeps `id` as a unique
   secondary key while moving `PRIMARY` to `code`, verifies the next implicit
   ID from an already-open peer, and proves a duplicate-key failure's consumed
   AUTO_INCREMENT value is not reused after forced `.shm` rebuild. An
   AUTO_INCREMENT descending primary-key replacement variant keeps `id` as a
   unique secondary key while moving `PRIMARY` to `code DESC`, verifies
   peer-visible `COLLATION = 'D'`, verifies the next implicit ID from an
   already-open peer, and proves a duplicate-key failure's consumed
   AUTO_INCREMENT value is not reused after forced `.shm` rebuild.
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
   Hook-build foreign-key action crash coverage kills parent update/delete
   writers after InnoDB prepares the referential-action cascade node but before
   executing child-side `row_update_cascade_for_mysql()`, proves live-peer
   cleanup remains busy, verifies no-live recovery restores the pre-action
   parent/child state, retries the same `ON UPDATE CASCADE` and
   `ON DELETE CASCADE`/`SET NULL` actions successfully, and checks final
   ownerless/native reopen before and after forced `.shm` rebuild. Hook-build
   post-action crash coverage also kills ordinary parent update/delete writers
   after a successful child-side referential action returns but before parent
   statement commit, proving no-live recovery rolls back uncommitted parent and
   child changes before the same actions are retried. Hook-build row-step crash
   coverage kills ordinary parent update/delete writers inside `row_upd_step()`
   before `row_upd()` applies the child-table update/delete and proves the same
   rollback, retry, and reopen behavior. Hook-build row-step after-crash
   coverage now uses multi-child ordinary FK action rows and kills parent
   update/delete writers after the first, second, third, or sixth matching child-side
   `row_upd()` succeeds, proving no-live recovery rolls back partial child-row
   action state before retry.
   Deep foreign-key cascade coverage now keeps a four-table
   `root -> level1 -> level2 -> level3` InnoDB chain active while another
   ownerless process updates the root primary key and deletes another root row,
   verifies an already-open peer observes the cascaded key at every dependent
   level, rejects a deepest-level insert against the old key with errno 1452,
   verifies the delete removes the matching row at every dependent level, and
   checks ownerless/native reopen before and after forced `.shm` rebuild.
   Generated-column foreign-key coverage now creates one stored generated child
   column that references a regular parent primary key and one stored generated
   parent key that is referenced by a regular child column, verifies
   already-open peer visibility for generated values and FK metadata, checks
   `ON UPDATE RESTRICT` failures with errno 1451, checks missing-parent
   insertion failures with errno 1452, verifies `ON DELETE CASCADE` removes
   child rows for both shapes, inserts valid rows after the cascade boundary,
   and checks ownerless/native reopen before and after forced `.shm` rebuild.
   Cyclic foreign-key coverage now creates a two-table FK cycle with
   `ON UPDATE RESTRICT`/`ON DELETE CASCADE`, verifies already-open peer
   visibility for both constraints and reciprocal edge values, checks
   restricted parent-key update failures with errno 1451, checks
   missing-parent failures with errno 1452, verifies deleting one side removes
   both rows through cyclic `ON DELETE CASCADE`, inserts a new valid cycle
   after the cascade boundary, checks MariaDB's cyclic `ON UPDATE CASCADE`
   rejection with errno 1451, and checks ownerless/native reopen before and
   after forced `.shm` rebuild.
   Cyclic foreign-key variant coverage now adds a three-table
   `ON DELETE CASCADE` cycle and a two-table `ON DELETE SET NULL` cycle,
   verifies already-open peer visibility for all constraints and edge values,
   checks deleting one side of the three-table cycle removes all rows, checks
   deleting either side of the set-null cycle preserves the reciprocal row with
   a `NULL` edge, inserts valid rows after both action boundaries, and checks
   ownerless/native reopen before and after forced `.shm` rebuild.
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
   rebuild. Hook-build same-schema foreign-key multi-pair rename crash coverage
   now kills that parent-through-temporary plus child-rename writer after native
   FK metadata and file moves but before ownerless dictionary finish, then
   verifies no-live recovery of the moved generated constraint identity,
   absence of the temporary parent name, FK enforcement, and ownerless/native
   reopen before and after forced `.shm` rebuild.
   Cross-schema foreign-key multi-pair rename coverage now moves both
   the referenced parent table and the child table owning an unnamed foreign key
   from `app` into another schema in one `RENAME TABLE` statement, verifies an
   already-open peer observes target-schema `CONSTRAINT_SCHEMA`,
   `UNIQUE_CONSTRAINT_SCHEMA`, target child/parent table names, and the moved
   generated `<child>_ibfk_1` constraint identity, checks `.frm` and `.ibd`
   movement between schema directories, inserts a valid child row through the
   moved child, rejects a missing-parent child insert with errno 1452, rejects
   deleting a still-referenced moved parent with errno 1451, and checks
   ownerless/native reopen before and after forced `.shm` rebuild.
   Hook-build cross-schema foreign-key multi-pair rename crash coverage now
   kills the moved parent/child writer after native FK metadata and file moves
   but before ownerless dictionary finish, then verifies no-live recovery of the
   moved generated constraint identity, target-schema parent/child files,
   FK enforcement, and ownerless/native reopen before and after forced `.shm`
   rebuild.
   Generated-column foreign-key policy coverage now verifies that indexed
   virtual generated child columns can participate in `CREATE TABLE` and
   `ALTER TABLE` foreign keys with `ON UPDATE RESTRICT` and
   `ON DELETE CASCADE`, rejects missing parents with errno 1452, rejects
   restricted parent updates with errno 1451, cascades parent deletes, rejects
   MariaDB-invalid generated-column action clauses with errno 1905, and checks
   ownerless/native reopen before and after forced `.shm` rebuild.
   Generated-column ALTER coverage verifies that same-kind stored and virtual
   generated-column expression replacement by another ownerless process refreshes
   already-open peer table metadata, recalculates existing generated values,
   remains active after peer DML changes base columns, and still preserves final
   ownerless/native reopen before and after forced `.shm` rebuild after the
   generated columns are dropped.
   Generated-column index DDL coverage verifies standalone ordinary and unique
   secondary indexes over deterministic stored and virtual generated columns,
   stored and virtual generated-column prefix indexes, a mixed-direction
   composite generated-column index, accepted explicit `NOCOPY`/`INPLACE`
   generated-column index add/drop option forms,
   already-open peer statistics refresh, forced-index reads while present,
   generated-value recalculation after peer DML changes base columns,
   forced-index failure after drop, and ownerless/native reopen before and
   after forced `.shm` rebuild.
   Indexed generated-column expression replacement coverage verifies that
   replacing deterministic stored and virtual generated-column expressions while
   ordinary secondary indexes over those columns remain present refreshes an
   already-open ownerless peer, preserves generated-column index metadata,
   recalculates forced-index predicates over the replacement expressions after
   peer DML, and survives ownerless/native reopen before and after forced
   `.shm` rebuild.
   Indexed generated-column expression duplicate-key policy coverage verifies
   that replacing stored or virtual generated-column expressions fails with
   errno 1062 when the replacement values would violate an existing unique
   generated-column index, leaves the original generated expression and unique
   index metadata usable by forced-index reads, and survives ownerless/native
   reopen before and after forced `.shm` rebuild.
   Generated-column primary-key policy coverage verifies that MariaDB-rejected
   create-time, replacement, and existing-column generated-column primary-key
   DDL fails with errno 1903, leaves no rejected tables, generated replacement
   columns, or primary-key metadata side effects, and preserves the surviving
   tables across ownerless/native reopen before and after forced `.shm`
   rebuild.
   Generated-column nondeterministic-expression policy coverage verifies that
   MariaDB-rejected stored generated `RAND()` expressions fail with errno 1901
   at create time and in `ALTER TABLE` add/modify paths without leaving table,
   column, expression, or row side effects, and verifies that a virtual
   generated `RAND()` expression remains non-indexed when standalone
   `CREATE INDEX` and `ALTER TABLE ... ADD INDEX` attempts fail with errno 1901,
   with ownerless/native reopen checks before and after forced `.shm` rebuild.
   Generated-column blocked-function policy coverage verifies representative
   aggregate, subquery, time-dependent, session-dependent, nondeterministic,
   crypto, statement-state, and user/version generated-column expression
   classes fail with errno 1901 at create time or in `ALTER TABLE` add/modify
   paths without leaving rejected tables, columns, expression replacements, or
   indexes behind. The same selector verifies that virtual `RAND()`,
   `CONNECTION_ID()`, and `DATABASE()` generated columns remain definable but
   non-indexable through standalone and alter-time index attempts, with
   ownerless/native reopen checks before and after forced `.shm` rebuild.
   Upstream blocked-function cases for MyLite-trimmed server utility functions
   such as `GET_LOCK()`, `SLEEP()`, and `UUID_SHORT()` remain covered by the
   server-utility SQL policy because they are rejected before MariaDB
   generated-column validation. Failed generated-column DDL crash recovery now
   kills representative generated-function and generated-primary-key writers
   after MariaDB validation failure but before ownerless dictionary finish, then
   verifies no rejected native metadata leaks and retry errno 1901/1903 remains
   stable. Exhaustive retained-function blocked-function replay and external
   oracle stress remain planned.
   Deterministic ownerless foreign-key graph stress now runs concurrent workers
   over shared InnoDB parent/child tables with `ON UPDATE CASCADE`,
   `ON DELETE CASCADE`, `ON DELETE SET NULL`, and `ON DELETE RESTRICT`, verifies
   missing-parent errno 1452 and restricted-delete errno 1451 failures, retries
   native lock-wait/deadlock outcomes (1205/1213) at whole-round boundaries,
   and checks aggregate/referential oracles through ownerless/native reopen
   before and after forced `.shm` rebuild. Stress worker arrays now use a shared
   child collector so an unexpected worker error preserves the first child
   status and reaps siblings rather than leaking ownerless workers until the
   CTest timeout. Unexpected non-retryable FK graph worker errors now also log
   the worker phase, deterministic root-id cursors, SQL, MariaDB errno, and
   MariaDB message before aborting so intermittent secondary-index or
   referential-action anomalies preserve enough state for the next fix slice.
   The same stress shape exposed the need for transient
   page-write transaction identities to hold first dirty user pages until SQL
   commit, now documented in `ownerless-transient-page-write-boundaries`, and
   later exposed a tracked secondary-index page publication boundary gap,
   documented in `ownerless-transaction-page-lsn-coverage`. The
   `ownerless-fk-graph-trace-export` slice adds deterministic SQL trace export
   for external harness input, and its worker trace now includes bounded
   `1205`/`1213` retry procedures so Docker-backed external MariaDB smoke can
   replay the deterministic FK graph. Hook-build FK action crash coverage now
   includes deterministic third- and sixth-child-row row-step-after-update faults, while
   long-running external MariaDB/RQG FK graph execution and broader randomized
   later-child-row referential-action crash fuzzing remain planned.
   Hook-build generated-column foreign-key action crash coverage reuses the
   pre-child-action and post-child-action fault points for MariaDB-supported
   stored generated child and generated referenced-column `ON DELETE CASCADE`
   shapes, kills parent delete writers before `row_update_cascade_for_mysql()`
   and after a successful child-side cascade returns before parent statement
   commit, plus a row-step fault inside `row_upd_step()` before `row_upd()`
   applies the child-table update/delete and after the first, second, third, or sixth matching
   child-side `row_upd()` succeeds in a multi-child cascade, proves live-peer
   cleanup remains busy, verifies no-live recovery restores generated FK rows,
   retries the same deletes successfully, and checks ownerless/native reopen
   before and after forced `.shm` rebuild. Exhaustive generated-column FK
   partial child-row modification crash fuzzing beyond the deterministic
   sixth-row boundary remains planned.
   CHECK constraint ALTER coverage adds two named table-level CHECK
   constraints from another ownerless process, verifies an already-open peer
   observes them through `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`, rejects
   invalid rows with errno 4025 while they exist, drops both constraints, and
   verifies the formerly invalid row shape can be inserted plus final
   absent-CHECK checks before and after forced `.shm` rebuild.
   Field/generated CHECK ALTER coverage adds a column-level `CHECK
   (value > 0)` plus a table-level `CHECK (generated_total >= value)` over a
   virtual generated column from another ownerless process, verifies an
   already-open peer observes `LEVEL='Column'` and `LEVEL='Table'`
   metadata, rejects invalid field and generated values with errno 4025 while
   they exist, drops both constraints, inserts formerly invalid rows, and
   verifies generated values plus absent-CHECK metadata through
   ownerless/native reopen before and after forced `.shm` rebuild. Hook-build
   crash coverage kills a CHECK ADD writer after native table-definition
   mutation but before ownerless dictionary finish and verifies the recovered
   CHECK metadata and enforcement state through ownerless/native reopen before
   and after forced `.shm` rebuild.
   Hook-build crash coverage also kills a CHECK-bearing
   `ALTER TABLE ... MODIFY value ... CHECK, ADD CONSTRAINT ... CHECK` writer
   after native table-definition mutation but before ownerless dictionary
   finish, then verifies recovered column-level CHECK metadata, recovered
   table-level generated-column CHECK metadata, errno 4025 enforcement for
   invalid field and generated values, generated value calculation, and
   ownerless/native reopen before and after forced `.shm` rebuild.
   Hook-build crash coverage also kills the field/generated CHECK
   `ALTER TABLE ... MODIFY value INT NOT NULL, DROP CONSTRAINT ...` writer
   before ownerless dictionary finish and verifies recovered absent
   column-level and generated-column CHECK metadata, formerly invalid writes,
   generated value calculation, and ownerless/native reopen before and after
   forced `.shm` rebuild.
   Hook-build crash coverage also kills a CHECK DROP writer at the same
   dictionary boundary and verifies recovered absent CHECK metadata plus
   formerly invalid rows through ownerless/native reopen before and after
   forced `.shm` rebuild.
   Charset-conversion coverage adds ownerless
   `ALTER TABLE ... CONVERT TO CHARACTER SET utf8mb4`, verifies an already-open
   peer observes `latin1` column metadata before conversion and `utf8mb4`
   metadata after conversion, inserts through the converted table, and verifies
   final converted metadata and rows before and after forced `.shm` rebuild.
   Hook-build crash coverage kills an `ALTER TABLE ... CONVERT TO CHARACTER SET`
   writer after native metadata/storage update but before ownerless dictionary
   finish, then verifies recovered charset/collation metadata, retained rows,
   post-recovery DML, ownerless/native reopen, and forced `.shm` rebuild.
   Row-format coverage adds ownerless
   `ALTER TABLE ... ROW_FORMAT=DYNAMIC` over a table created as
   `ROW_FORMAT=COMPACT`, verifies an already-open peer observes the native
   `INNODB_SYS_TABLES.ROW_FORMAT` transition, inserts through the rebuilt
   table, and verifies final metadata and rows before and after forced `.shm`
   rebuild. Compressed row-format coverage adds ownerless
   `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` plus focused
   `KEY_BLOCK_SIZE=1`, `KEY_BLOCK_SIZE=2`, `KEY_BLOCK_SIZE=4`, and
   `KEY_BLOCK_SIZE=16` rebuilds, verifies an
   already-open peer observes the
   native compressed row-format transition, inserts a prepared BLOB row through
   the rebuilt table, and verifies final compressed metadata plus native
   `FIL_PAGE_TYPE_ZBLOB`/`ZBLOB2` page evidence before and after forced `.shm`
   rebuild. The compressed selectors also assert that the post-peer rebuild
   parent window records ownerless refresh calls but does not enable
   page-version reads, proving the conservative native-read boundary while
   rebuild generation is not encoded in the page-version key.
   Table-comment coverage adds ownerless
   `ALTER TABLE ... COMMENT='ownerless updated comment'`, verifies an
   already-open peer observes `information_schema.TABLES.TABLE_COMMENT`
   before and after the ALTER, inserts through the table after the metadata
   boundary, and verifies final comment metadata and rows before and after
   forced `.shm` rebuild. Hook-build crash coverage kills an
   `ALTER TABLE ... COMMENT` writer after native metadata update but before
   ownerless dictionary finish, then verifies recovered comment metadata,
   retained rows, post-recovery DML, ownerless/native reopen, and forced
   `.shm` rebuild.
   Force-rebuild coverage adds ownerless `ALTER TABLE ... FORCE`, verifies an
   already-open peer can continue reading through a secondary index after the
   rebuild boundary, inserts through the rebuilt table, and verifies final
   native table metadata, secondary-index metadata, and rows before and after
   forced `.shm` rebuild.
   Column-default coverage adds ownerless `ALTER COLUMN ... SET DEFAULT` and
   `ALTER COLUMN ... DROP DEFAULT`, verifies an already-open peer uses the
   original defaults, then the changed defaults, then fails when omitting a
   NOT NULL column after its default is dropped, and verifies final metadata and
   rows before and after forced `.shm` rebuild. Hook-build crash coverage now
   kills an `ALTER COLUMN ... SET DEFAULT` writer after native metadata update
   but before ownerless dictionary finish, then verifies recovered default
   metadata, post-recovery default-backed inserts, ownerless/native reopen, and
   forced `.shm` rebuild.
   Column-idempotent crash coverage kills duplicate
   `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing
   `ALTER TABLE ... DROP COLUMN IF EXISTS` no-op writers after MariaDB success
   but before ownerless dictionary finish, then verifies original column/default
   preservation, missing-column absence, MariaDB 1060/1091 retry errno,
   post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild.
   Column missing-`IF EXISTS` crash coverage kills missing
   `ALTER TABLE ... MODIFY COLUMN IF EXISTS`,
   `ALTER TABLE ... RENAME COLUMN IF EXISTS`,
   `ALTER TABLE ... CHANGE COLUMN IF EXISTS`,
   `ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and
   `ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` no-op writers after
   MariaDB success but before ownerless dictionary finish, then verifies
   original column/default preservation, missing and attempted renamed/changed
   column absence, MariaDB 1054 retry errno, post-recovery writes,
   ownerless/native reopen, and forced `.shm` rebuild. Focused expression
   variants kill missing `RENAME COLUMN IF EXISTS`, `CHANGE COLUMN IF EXISTS`,
   and `ALTER COLUMN IF EXISTS SET/DROP DEFAULT` on tables with stored and
   virtual generated columns plus CHECK constraints, verifying the real column,
   generated values, real-column defaults, CHECK enforcement, missing attempted
   names, and plain retry errno 1054 remain stable.
   Special-index policy coverage
   rejects ownerless `FULLTEXT` and `SPATIAL` index DDL through top-level
   `CREATE INDEX`, `CREATE INDEX IF NOT EXISTS`,
   `ALTER TABLE ... ADD INDEX`, `ALTER TABLE ... ADD INDEX IF NOT EXISTS`, and
   inline `CREATE TABLE` definitions before MariaDB creates special index
   metadata or native storage.
   Partition policy coverage rejects ownerless `CREATE TABLE ... PARTITION BY`,
   `CREATE TABLE ... SUBPARTITION BY`, `ALTER TABLE ... PARTITION BY`,
   partition-maintenance `ALTER TABLE` forms covering add, drop, rebuild,
   optimize, analyze, check, repair, coalesce, truncate, and reorganize,
   `ALTER TABLE ... EXCHANGE PARTITION`,
   `ALTER TABLE ... CONVERT PARTITION ... TO TABLE`,
   `ALTER TABLE ... CONVERT TABLE ... TO PARTITION`, and
   `ALTER TABLE ... REMOVE PARTITIONING` before MariaDB creates partition
   metadata, conversion targets, or native partition files.
   Table-directory policy coverage rejects ownerless `CREATE TABLE` and
   `ALTER TABLE` `DATA DIRECTORY`/`INDEX DIRECTORY` options, including a
   partition-level `DATA DIRECTORY` spelling, before MariaDB creates external
   native table paths.
   Tablespace-management policy coverage rejects ownerless
   `ALTER TABLE ... DISCARD TABLESPACE` and
   `ALTER TABLE ... IMPORT TABLESPACE` before MariaDB detaches or imports
   native InnoDB tablespace files.
   Table storage-option policy coverage rejects ownerless create-time and
   alter-time `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`,
   `ENCRYPTION_KEY_ID`, and table `TABLESPACE` options before MariaDB enters
   unproven native page-compression, encryption, or table-option file-layout
   paths, including representative idempotent, replacement, and temporary
   create-table spellings, while quoted columns using those words and CTAS
   result aliases remain ordinary identifiers.
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
   concurrent DDL/DML evidence. The `ownerless-ddl-stress-trace-export` slice
   adds deterministic SQL trace export for external harness input using the
   same DDL/DML schedule; full external-oracle randomized DDL execution remains
   planned. The `ownerless-ddl-lifecycle-trace-export` slice adds a sharper
   deterministic DDL lifecycle trace for create, rename, truncate, force
   rebuild, drop, same-name recreate, replacement-copy DDL, cross-schema
   rename, and dropped-schema file lifecycle with final metadata, aggregate,
   and InnoDB `SPACE` identity oracle SQL, plus bounded reader retry handling
   for external MariaDB `1020`, `1205`, `1213`, and SQLSTATE `40001`
   contention. The `ownerless-ddl-cross-schema-trace` follow-up extends that
   trace with per-round cross-schema rename `SPACE` identity checks and
   dropped-schema absence checks.

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
   table stress with `MYLITE_OWNERLESS_TEMP_STRESS_ROUNDS=40`, all with
   forced `.shm` rebuild and native exclusive reopen checks. The
   `ownerless-page-write-timeout-retry` slice specifically covers the amplified
   DDL stale-`DB_LOCK_WAIT_TIMEOUT` transaction-start assertion and the
   temporary-table `dict_hdr_get_new_id()` null-page segfault caused by
   low-level ownerless page-write timeouts that could not safely propagate as
   SQL errors. The
   `ownerless-independent-table-stress-trace-export` slice adds
   `tools/ownerless-independent-table-stress-trace`, which emits schema,
   per-table worker SQL, live-reader SQL, an expected aggregate/per-table
   oracle, and a manifest for external MariaDB/RQG-style runners using the
   same deterministic independent-table schedule. The
   `ownerless-temporary-table-stress-trace-export` slice adds
   `tools/ownerless-temporary-table-stress-trace`, which emits schema,
   per-worker temporary-table SQL, post-worker permanent-table SQL, an expected
   durable-table oracle, and a manifest for external MariaDB/RQG-style runners
   using the same deterministic same-name temporary-table schedule. The
   `ownerless-ddl-stress-trace-export` slice adds
   `tools/ownerless-ddl-stress-trace`, which emits schema, DDL worker SQL, DML
   worker SQL, live-reader SQL, an expected aggregate/metadata oracle, and a
   manifest for external MariaDB/RQG-style runners using the same deterministic
   create/alter/index/rename/truncate/drop plus DML schedule. The
   `ownerless-ddl-stress-seed-suite` slice adds deterministic nonzero DDL
   stress seed variants plus a multi-seed trace-runner bridge for generated
   external-oracle coverage without treating that as full randomized RQG. The
   `ownerless-seed211-trace-checks` follow-up adds seed `211` to the
   dependency-free DDL seed-suite CTest check, and the
   `ownerless-seed211-external-replay` follow-up extends the Docker-backed DDL
   seed replay evidence to seed `211`. The
   `ownerless-ddl-seed-external-replay` slice adds an opt-in disposable
   MariaDB 11.8 Docker wrapper and records rounds-8 replay evidence for seeds
   `0`, `17`, `83`, and `211`, with empty final oracle stderr and
   `ownerless_ddl_stress_trace_check=ok` for each seed. The
   `ownerless-ddl-lifecycle-trace-export` slice adds
   `tools/ownerless-ddl-lifecycle-trace`, which emits schema, a DDL lifecycle
   worker, repeatable-snapshot reader SQL, an expected final recreated-table
   metadata/value oracle, replacement-copy metadata/value oracles, per-round
   same-name recreated and cross-schema-renamed InnoDB `SPACE` identity
   checks, dropped-schema absence checks, bounded reader retry handling for
   external MariaDB contention, and a manifest for external MariaDB/RQG-style
   runners. It also runs shared-table checksum stress with
   `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=48`, mixing direct SQL and
   reusable prepared-statement writers with bounded retry for ownerless
   statement-lock busy, MariaDB lock-wait, and MariaDB deadlock errors while
   checking sum, version, and weighted-sum aggregates against a deterministic
   oracle before and after forced `.shm` rebuild through ownerless and native
   exclusive reopen. Larger same-table checksum runs remain opt-in until
   ownerless same-table writer fairness is improved. The
   `ownerless-checksum-stress-trace-export` slice adds
   `tools/ownerless-checksum-stress-trace`, which emits schema, per-worker SQL,
   live-reader SQL, an expected count/sum/version/weighted-sum oracle, and a
   manifest for external MariaDB/RQG-style runners using the same deterministic
   checksum schedule. It also runs pseudo-random shared-table transaction
   stress with
   `MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS=120`, padded worker-owned row
   partitions, savepoint rollback, full transaction rollback, bounded rollback
   and retry for MariaDB lock-wait/deadlock errors, a live aggregate reader, final
   sum/version/weighted-sum oracles, and forced `.shm` rebuild plus native
   exclusive reopen checks. A focused three-round registered guard now keeps the
   rollback/retry handoff failure visible in the normal production ownerless SQL
   CTest set. The `ownerless-random-tx-trace-export` slice adds
   `tools/ownerless-random-tx-trace`, which emits schema, per-worker SQL, an
   expected aggregate oracle, and a manifest for external MariaDB/RQG-style
   runners using the same deterministic random transaction schedule when
   `--seed 0` is used. The `ownerless-random-tx-seed-suite` slice adds
   deterministic nonzero seed variants plus a multi-seed trace-runner bridge for
   generated external-oracle coverage without treating that as full randomized
   RQG. The `ownerless-seed211-trace-checks` follow-up adds seed `211` to the
   dependency-free random-transaction seed-suite CTest check, and the
   `ownerless-seed211-external-replay` follow-up adds a disposable MariaDB 11.8
   random-transaction seed wrapper plus rounds-8 replay evidence for seeds `0`,
   `17`, `83`, and `211`. The `ownerless-external-seed-sweep` slice adds one
   opt-in disposable MariaDB 11.8 wrapper for both seeded suites, dependency-free
   CTest command-plan validation for seeds `0` through `15` at rounds `3`, and
   focused Docker-backed replay evidence for random transaction and DDL seed
   suites over seeds `0` through `7` at rounds `4` without treating that as full
   randomized RQG. The `ownerless-external-seed-range-checks` follow-up makes
   the seed-sweep manifest self-describing for check/replay mode, per-suite
   rounds, and per-suite output paths, and broadens dependency-free
   command-plan validation to seeds `0` through `15` at rounds `3`. The
   `ownerless-external-seed-wide-check` follow-up adds a second dependency-free
   command-plan validation over seeds `0` through `31` at rounds `2`, plus
   bounded Docker-backed replay evidence for seeds `8` through `15` at rounds
   `2`. The `ownerless-external-seed-16-31-replay` follow-up records
   Docker-backed MariaDB 11.8 replay for seeds `16` through `31` at rounds `2`
   across random transaction, DDL stress, and FK graph seeded suites, with FK
   graph seeds `17`, `21`, and `25` recovering on attempt `2` after transient
   raw MariaDB `1213` deadlock exits. The
   `ownerless-external-seed-32-63-replay` follow-up records Docker-backed
   MariaDB 11.8 replay for seeds `32` through `63` at rounds `2` across the
   same three seeded suites, with FK graph seeds `34`, `35`, `36`, and `37`
   recovering on attempt `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-64-95-replay` follow-up records the next
   Docker-backed MariaDB 11.8 replay window for seeds `64` through `95` at
   rounds `2` across the same three seeded suites, with FK graph seeds `64`,
   `81`, and `85` recovering on attempt `2` after transient raw MariaDB `1213`
   deadlock exits. The `ownerless-external-seed-96-127-replay` follow-up
   records the next replay window for seeds `96` through `127` at rounds `2`,
   with FK graph seeds `106`, `113`, and `118` recovering on attempt `2` after
   transient raw MariaDB `1213` deadlock exits. The
   `ownerless-external-seed-128-159-replay` follow-up records the next replay
   window for seeds `128` through `159` at rounds `2`, with FK graph seeds
   `129` and `132` recovering on attempt `2` after transient raw MariaDB
   `1213` deadlock exits. The `ownerless-external-seed-160-191-replay`
   follow-up records the next replay window for seeds `160` through `191` at
   rounds `2`, with FK graph seeds `160`, `162`, `164`, `168`, `170`, and
   `176` recovering on attempt `2` after transient raw MariaDB `1213` deadlock
   exits. The `ownerless-external-seed-192-223-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `0` through `63` and
   records the next replay window for seeds `192` through `223` at rounds `2`,
   with FK graph seeds `211`, `213`, `218`, and `219` recovering on attempt `2`
   after transient raw MariaDB `1213` deadlock exits. The
   `ownerless-external-seed-224-255-replay` follow-up records the next replay
   window for seeds `224` through `255` at rounds `2`, exposes
   `--fk-graph-replay-attempts` on the combined wrapper, and uses an explicit
   FK graph whole-seed retry budget of `10` so seeds `229`, `232`, `233`,
   `234`, `239`, `242`, and `252` can recover after transient raw MariaDB
   `1213` deadlock exits. The `ownerless-external-seed-256-287-replay`
   follow-up records the next replay window for seeds `256` through `287` at
   rounds `2`, using the same explicit FK graph whole-seed retry budget of
   `10` so seeds `260`, `267`, `277`, and `280` can recover after transient
   raw MariaDB `1213` deadlock exits. The
   `ownerless-external-seed-288-319-replay` follow-up records the next replay
   window for seeds `288` through `319` at rounds `2`, using the same explicit
   FK graph whole-seed retry budget of `10` so seeds `288`, `294`, `298`,
   `304`, `308`, and `310` can recover after transient raw MariaDB `1213`
   deadlock exits. The `ownerless-external-seed-320-351-replay` follow-up
   records the next replay window for seeds `320` through `351` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seed
   `344` can recover on attempt `3` and seed `348` can recover on attempt `2`
   after transient raw MariaDB `1213` deadlock exits. The
   `ownerless-external-seed-352-383-replay` follow-up records the next replay
   window for seeds `352` through `383` at rounds `2`, using the same explicit
   FK graph whole-seed retry budget of `10` so seed `354` can recover on
   attempt `4`, seeds `357` and `365` can recover on attempt `3`, and seeds
   `358`, `362`, `363`, `368`, `369`, `371`, `375`, and `379` can recover on
   attempt `2` after transient raw MariaDB `1213` deadlock exits. The
   `ownerless-external-seed-384-415-replay` follow-up records the next replay
   window for seeds `384` through `415` at rounds `2`, using the same explicit
   FK graph whole-seed retry budget of `10` so seed `403` can recover on
   attempt `3`, seed `405` can recover on attempt `2`, seed `406` can recover on
   attempt `5`, seed `408` can recover on attempt `3`, and seed `413` can
   recover on attempt `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-416-447-replay` follow-up records the next
   replay window for seeds `416` through `447` at rounds `2`, using the same
   explicit FK graph whole-seed retry budget of `10` so seed `417` can recover
   on attempt `3`, seeds `418` and `419` can recover on attempt `2`, seed
   `420` can recover on attempt `5`, seed `423` can recover on attempt `3`,
   and seeds `425`, `434`, and `436` can recover on attempt `2` after
   transient raw MariaDB `1213` deadlock exits. The
   `ownerless-external-seed-448-479-replay` follow-up records the next replay
   window for seeds `448` through `479` at rounds `2`, using the same explicit
   FK graph whole-seed retry budget of `10` so seed `449` can recover on
   attempt `3`, seed `456` can recover on attempt `2`, seeds `463`, `466`,
   `467`, `471`, and `475` can recover on attempt `3`, and seed `477` can
   recover on attempt `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-480-511-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `64` through `127` and
   records the next replay window for seeds `480` through `511` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seed
   `481` can recover on attempt `2`, seed `483` can recover on attempt `3`,
   seed `486` can recover on attempt `2`, seed `490` can recover on attempt
   `5`, seed `492` can recover on attempt `2`, seed `493` can recover on
   attempt `4`, seed `500` can recover on attempt `4`, seed `503` can recover
   on attempt `2`, seed `505` can recover on attempt `3`, seeds `507`, `509`,
   and `510` can recover on attempt `2`, and seed `511` can recover on attempt
   `3` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-512-543-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `128` through `191` and
   records the next replay window for seeds `512` through `543` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seed
   `519` can recover on attempt `3`, and seeds `522`, `523`, `525`, `527`,
   `528`, `536`, and `542` can recover on attempt `2` after transient raw
   MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-544-575-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `192` through `255` and
   records the next replay window for seeds `544` through `575` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seed
   `545` can recover on attempt `5`, seed `548` can recover on attempt `4`,
   seeds `549` and `557` can recover on attempt `3`, and seeds `546`, `560`,
   `562`, `564`, and `575` can recover on attempt `2` after transient raw
   MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-576-607-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `256` through `319` and
   records the next replay window for seeds `576` through `607` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seeds
   `581`, `586`, and `605` can recover on attempt `4`, seeds `576`, `584`,
   `595`, `599`, and `604` can recover on attempt `3`, and seeds `578`, `579`,
   `582`, `587`, `589`, `590`, and `606` can recover on attempt `2` after
   transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-608-639-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `320` through `383` and
   records the next replay window for seeds `608` through `639` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seeds
   `609`, `613`, and `621` can recover on attempt `3`, and seeds `608`, `616`,
   `618`, `626`, `633`, and `634` can recover on attempt `2` after transient
   raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-640-671-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `384` through `447` and
   records the next replay window for seeds `640` through `671` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seed
   `657` can recover on attempt `4`, seed `654` can recover on attempt `3`,
   and seeds `641`, `644`, `647`, `649`, `652`, `655`, `667`, `669`, and
   `670` can recover on attempt `2` after transient raw MariaDB `1213`
   deadlock exits.
   The `ownerless-external-seed-672-703-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `448` through `511` and
   records the next replay window for seeds `672` through `703` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seed
   `701` can recover on attempt `6`, seed `689` can recover on attempt `5`,
   seed `678` can recover on attempt `4`, seeds `693`, `698`, `699`, `700`,
   and `702` can recover on attempt `3`, and seeds `672`, `692`, `694`, and
   `703` can recover on attempt `2` after transient raw MariaDB `1213`
   deadlock exits.
   The `ownerless-external-seed-704-735-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `512` through `575` and
   records the next replay window for seeds `704` through `735` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seed
   `722` can recover on attempt `5`, seeds `708`, `725`, and `726` can recover
   on attempt `3`, and seeds `704`, `706`, `713`, `715`, `718`, `724`, `728`,
   and `732` can recover on attempt `2` after transient raw MariaDB `1213`
   deadlock exits.
   The `ownerless-external-seed-736-767-replay` follow-up widens the
   dependency-free seed-sweep CTest window to seeds `576` through `639` and
   records the next replay window for seeds `736` through `767` at rounds `2`,
   using the same explicit FK graph whole-seed retry budget of `10` so seeds
   `737`, `738`, `755`, and `764` can recover on attempt `4`, seeds `742`,
   `745`, and `747` can recover on attempt `3`, and seeds `736`, `744`, `746`,
   `750`, `757`, and `765` can recover on attempt `2` after transient raw
   MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-640-703-check` follow-up widens the persistent
   dependency-free seed-sweep CTest window to seeds `640` through `703`,
   matching the already-recorded `640` through `671` and `672` through `703`
   Docker-backed replay windows without adding Docker to default CI.
   The `ownerless-external-seed-704-767-check` follow-up catches that
   persistent dependency-free window up to seeds `704` through `767`, matching
   the already-recorded `704` through `735` and `736` through `767`
   Docker-backed replay windows without adding Docker to default CI.
   The `ownerless-external-seed-768-799-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `768` through `799` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `778` can
   recover on attempt `4`, seeds `774`, `780`, and `788` can recover on attempt
   `3`, and seeds `769`, `777`, `785`, `787`, and `794` can recover on attempt
   `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-800-831-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `800` through `831` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `801` and
   `803` can recover on attempt `4`, seeds `811` and `829` can recover on
   attempt `3`, and seeds `802`, `806`, `823`, `826`, and `830` can recover on
   attempt `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-832-863-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `832` through `863` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `849` can
   recover on attempt `4`, seeds `834` and `852` can recover on attempt `3`,
   and seeds `833`, `837`, `845`, `846`, and `851` can recover on attempt `2`
   after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-864-895-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `864` through `895` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `887` can
   recover on attempt `5`, seed `872` can recover on attempt `3`, and seeds
   `865`, `870`, `878`, `879`, `880`, `886`, and `891` can recover on
   attempt `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-896-927-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `896` through `927` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `905` can
   recover on attempt `5`, seeds `904`, `910`, and `918` can recover on
   attempt `3`, and seeds `899` and `908` can recover on attempt `2` after
   transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-928-959-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `928` through `959` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `957` can
   recover on attempt `4`, seed `939` can recover on attempt `3`, and seeds
   `932`, `934`, `946`, and `956` can recover on attempt `2` after transient
   raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-960-991-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `960` through `991` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `968` can
   recover on attempt `5`, seeds `963`, `969`, and `985` can recover on
   attempt `4`, seeds `965`, `972`, `976`, `979`, and `984` can recover on
   attempt `3`, and seeds `975`, `978`, `981`, and `982` can recover on
   attempt `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-992-1023-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `992` through `1023` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `992`,
   `993`, `994`, `997`, `999`, `1004`, `1011`, `1015`, `1020`, and `1022`
   can recover on attempt `2` after transient raw MariaDB `1213` deadlock
   exits; no seed needed a third attempt in this window.
   The `ownerless-external-seed-1024-1055-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1024` through `1055` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `1052` can
   recover on attempt `7`, seeds `1026` and `1054` can recover on attempt
   `3`, and seeds `1029`, `1031`, `1042`, `1043`, `1053`, and `1055` can
   recover on attempt `2` after transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-1056-1087-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1056` through `1087` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `1067` and
   `1074` can recover on attempt `3`, and seeds `1060`, `1063`, `1068`,
   `1070`, `1078`, and `1080` can recover on attempt `2` after transient raw
   MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-1088-1119-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1088` through `1119` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seed `1104` can
   recover on attempt `6`, seeds `1096` and `1097` can recover on attempt
   `5`, seed `1114` can recover on attempt `4`, seeds `1095` and `1103` can
   recover on attempt `3`, and seeds `1088`, `1094`, `1105`, `1108`, `1111`,
   and `1112` can recover on attempt `2` after transient raw MariaDB `1213`
   deadlock exits.
   The `ownerless-external-seed-1120-1151-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1120` through `1151` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `1120`,
   `1121`, and `1130` can recover on attempt `3`, and seeds `1123`, `1128`,
   `1133`, `1136`, `1150`, and `1151` can recover on attempt `2` after
   transient raw MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-1152-1183-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1152` through `1183` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `1152`,
   `1155`, and `1157` can recover on attempt `4`, seeds `1156`, `1173`, and
   `1183` can recover on attempt `3`, and seeds `1153`, `1159`, `1167`, and
   `1170` can recover on attempt `2` after transient raw MariaDB `1213`
   deadlock exits.
   The `ownerless-external-seed-1184-1215-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1184` through `1215` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `1185` and
   `1203` can recover on attempt `2` after transient raw MariaDB `1213`
   deadlock exits.
   The `ownerless-external-seed-1216-1247-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1216` through `1247` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `1221`,
   `1223`, `1225`, `1228`, `1231`, `1232`, `1233`, and `1246` can recover on
   attempt `2`, seeds `1222`, `1234`, `1236`, and `1247` can recover on
   attempt `3`, and seed `1224` can recover on attempt `4` after transient raw
   MariaDB `1213` deadlock exits.
   The `ownerless-external-seed-1248-1279-replay` follow-up advances the next
   dependency-free seed-sweep CTest window to seeds `1248` through `1279` and
   records Docker-backed replay for the same window at rounds `2`, using the
   same explicit FK graph whole-seed retry budget of `10` so seeds `1249`,
   `1256`, `1262`, `1263`, `1267`, `1272`, and `1274` can recover on attempt
   `2`, seeds `1250` and `1278` can recover on attempt `3`, and seed `1270`
   can recover on attempt `4` after transient raw MariaDB `1213` deadlock
   exits.
   Separate foreign-key graph stress
   coverage runs with
   `MYLITE_OWNERLESS_FK_GRAPH_STRESS_ROUNDS=48`, concurrent ownerless workers
   over shared `CASCADE`, `SET NULL`, and `RESTRICT` foreign-key edges,
   bounded retry for MariaDB 1205/1213, deterministic aggregate/referential
   oracles, missing-parent and restricted-delete error checks, and forced `.shm`
   rebuild plus native exclusive reopen checks. The
   `ownerless-fk-graph-trace-export` slice adds
   `tools/ownerless-fk-graph-trace`, which emits schema, per-worker SQL,
   expected-error probes, an expected aggregate/referential oracle, and a
   manifest for external MariaDB/RQG-style runners using the same deterministic
   foreign-key graph schedule. The `ownerless-fk-graph-seed-suite` follow-up
   adds `--seed` support while preserving seed `0` as the deterministic product
   graph schedule, adds `tools/ownerless-fk-graph-seed-suite`, adds an optional
   disposable MariaDB FK graph seed-smoke wrapper, and extends the combined
   external seed sweep so FK graph generated traces are validated over the same
   dependency-free seed windows as random transaction and DDL stress. Bounded
   Docker-backed MariaDB 11.8 replay evidence covered seeds `0`, `17`, `83`,
   and `211` at rounds `2`, with the suite's whole-seed replay attempts
   recovering transient raw `1213` exits for seeds `17` and `83`. The
   `ownerless-stress-child-failure-cleanup`
   slice adds a focused stress-harness selector and shared worker collector so a
   failing child terminates and reaps still-running stress siblings instead of
   hiding the first failure behind a 900-second CTest timeout. The
   `ownerless-transient-page-write-boundaries` slice keeps explicit-transaction
   page-write locks acquired under a transient ownerless page-write identity
   transaction-scoped and prepares later same-tablespace writable persistent
   pages before B-tree navigation, preventing first dirty pages or stale
   secondary pages from crossing the SQL transaction boundary while preserving
   autocommit DDL/truncate release behavior. The
   `ownerless-transaction-page-lsn-coverage` slice widens ownerless transaction
   page publication to the highest valid LSN observed among the tracked
   transaction pages before flushing and releasing page-write locks, preventing
   a committed clustered row from outrunning its secondary-index page version.
   It also refreshes clean local pages for DML/locking-read current reads inside
   explicit ownerless transactions, preserving dirty local pages while avoiding
   zero-row parent updates caused by stale search pages. Native InnoDB
   foreign-key checks now prepare that ownerless current-read boundary before
   opening FK B-tree cursors, then refresh and reopen the FK cursor page so
   parent-side referential actions resolve peer-created child secondary and
   clustered records without a SQL-level `information_schema` parent probe.
   The
   `ownerless-online-ddl-option-matrix` slice adds deterministic peer-refresh
   and reopen coverage for accepted ordinary secondary-index
   `NOCOPY`/`LOCK=SHARED`, `NOCOPY`/`LOCK=EXCLUSIVE`, and
   `INPLACE`/`LOCK=EXCLUSIVE` add/drop option combinations. The
   `ownerless-online-unique-index-ddl-options` slice adds matching focused
   coverage for a unique secondary-index `INPLACE`/`LOCK=SHARED` add/drop
   pair with duplicate-key enforcement through an already-open peer. The
   `ownerless-runtime-startup-serialization` slice serializes ownerless native
   startup, connection, core `mysql.*` compatibility-table bootstrap, and
   dictionary-generation initialization, so concurrent openers do not race
   InnoDB redo startup; the opener creates `concurrency/` before taking that
   startup lock. Fresh ordinary exclusive read/write opens now skip
   `concurrency/` creation, SHM/WAL/checkpoint preparation, process-slot
   allocation, ownerless redo evidence capture, and ownerless close cleanup
   unless explicit ownerless/shared-readonly flags are present or durable
   ownerless runtime files already exist. Ownerless startup failures are
   retried a bounded number of
   times only after ending partial MariaDB embedded startup state and restoring
   the saved 12 KiB redo startup prefix when its checkpoint pages pass MariaDB
   startup validation, or the captured prefix fallback; ordinary native
   read/write reopen uses the same failure-then-restore retry path after
   ownerless activity, and can arm only the ownerless uncheckpointed
   file-operation recovery mode during native startup when retained page WAL,
   the checksummed native file-op checkpoint marker, or a valid
   `mylite-redo-header.bin` backup proves prior ownerless redo/checkpoint
   suppression; hook-only SQL coverage corrupts redo-header backup magic,
   format, header size, payload size, recorded redo size, saved prefix, and
   truncation boundaries and proves those files do not arm the ordinary-open
   recovery bridge. Final no-live ownerless read/write shutdown
   uses the same startup lock to publish native `FILE_CHECKPOINT` evidence for
   completed DDL file-operation redo, and focused SQL coverage proves
   `ALTER TABLE ... AUTO_INCREMENT` sets the native file-op checkpoint marker
   while a live peer prevents final drain; final no-live close drains that
   marker and also drains a stale native file-op checkpoint marker even when
   `.ckpt` has no page-visible LSN or WAL to compact; marker reads prefer the
   highest valid marker generation and treat corrupt marker-record-only evidence
   as checkpoint-needed, so a torn clear can cause an extra checkpoint but cannot
   suppress required native drain; ownerless dictionary DDL now also persists
   the same marker after native `FILE_*` redo evidence but before the
   `dictionary-before-finish` crash hook can interrupt dictionary finish, with
   focused `RENAME TABLE` `FILE_RENAME`, `CREATE TABLE ... LIKE`
   `FILE_CREATE`, CTAS populated `FILE_CREATE`, `TRUNCATE TABLE` native
   truncate/recreate, `DROP TABLE` `FILE_DELETE`, and replacement-copy
   `CREATE OR REPLACE TABLE ... LIKE`/`CREATE OR REPLACE TABLE ... AS SELECT`
   marker coverage plus representative `ALTER TABLE ... FORCE` and
   `ALTER TABLE ... ROW_FORMAT=DYNAMIC` rebuild marker coverage, plus
   compressed `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` rebuild marker coverage
   at the same prefinish crash boundary, now extended to the existing
   compressed key-block `1`/`2`/`4`/`16` crash variants so every focused
   compressed key-block crash selector proves the native file-op marker while
   the live peer still prevents final drain. Plain non-temporary `CREATE TABLE`
   prefinish crash coverage now publishes a
   per-owner recoverable dictionary marker after the durable file-op marker, so
   dead-owner cleanup can finish that created-table dictionary generation while
   another peer remains live and leave native file-op marker drain to the
   existing no-live checkpoint path. Non-temporary `CREATE TABLE ... LIKE`
   prefinish crash coverage now uses a separate recoverable dictionary marker
   to provide the same live-peer recovery for the copied empty destination
   table and secondary-index metadata. Focused no-definition-list CTAS
   prefinish crash coverage uses a separate recoverable dictionary marker to
   provide live-peer recovery for the populated destination table. Replacement,
   rename, truncate, drop, rebuild, schema, view, trigger, and foreign-key
   multi-DDL live-peer recovery remain planned. Final no-live close
   forces native checkpoint
   proof for retained page-version WAL
   after active pins release, restores the 12 KiB redo startup prefix if
   embedded teardown leaves `ib_logfile0` without startup-checkpoint evidence,
   and
   lets ownerless uncheckpointed file-operation recovery resolve relative FILE
   redo paths, normalize datadir-prefixed `.ibd` FILE redo names including the
   observed leading-separator-stripped datadir form before prepending the active
   datadir, synthesize a missing checkpoint boundary only at a clean
   EOF/no-corrupt-FS recovery boundary, and drop the redo latch around
   doublewrite recovery before reacquiring it. Focused hook coverage now also
   forces a native checkpoint, clears the ownerless file-op redo flag, updates a
   file-per-table InnoDB table, and observes the flag set again. That proves
   ordinary post-checkpoint DML can reach MariaDB's `FILE_MODIFY` redo path,
   while durable marker coverage for every DML-origin `FILE_MODIFY` case
   remains unclaimed. Bounded DML marker follow-ups now consume the same native
   file-op redo flag after successful autocommit non-DDL ownerless writes and
   successful explicit transaction `COMMIT` after local writes, and persist a
   DML-specific checkpoint-needed marker when checkpointed DML emits
   file-operation redo. That marker forces native checkpoint drain but does not
   relax no-live user-page LSN/payload proof; the existing native file-op marker
   remains the proof-relaxing dictionary DDL/file-lifecycle marker. Focused SQL
   coverage forces a checkpoint, updates file-per-table InnoDB tables through
   autocommit, single-owner explicit-transaction, and idle-peer explicit
   transaction commit shapes, observes the DML marker before close or before
   final peer release as appropriate, drains the autocommit and single-owner
   explicit DML markers on close, retains peer-observed explicit DML marker/WAL
   evidence while the peer is live, then drains it after final peer exit when
   no-live native checkpoint and page-image proof succeeds, and verifies
   ownerless plus ordinary native reopen after forced `.shm` rebuild. Ordinary
   native opens that use retained ownerless WAL retire the startup page-version
   visibility before later native write or locking-read statements so stale
   retained page images cannot shadow ordinary writes.
   Successful explicit transaction rollback after local writes is covered as a
   separate outcome: it leaves both native file-op markers clear, consumes the
   process-local ownerless InnoDB file-op redo flag, and preserves the
   pre-transaction row after forced `.shm` rebuild plus ordinary native reopen.
   `ROLLBACK TO SAVEPOINT` after checkpointed local DML now restores
   MyLite's handle-local write-state from the savepoint boundary, consumes
   process-local file-op redo when no earlier local write survives the
   rollback, leaves both file-op markers clear after the later `COMMIT`, and
   preserves the pre-savepoint row through forced `.shm` rebuild plus ordinary
   native reopen.
   The retained-earlier-write branch is covered separately: when a
   checkpointed write before the savepoint survives rollback of a later
   checkpointed write, the process-local file-op redo flag remains set through
   `ROLLBACK TO`, the later `COMMIT` publishes the DML-specific marker, no-live
   close drains it after native checkpoint proof, and ownerless/native reopen
   sees only the surviving pre-savepoint row image.
   The killed-session follow-up proves the same post-savepoint rollback path
   after a successful later `COMMIT` and `_exit(0)` before `mylite_close()`:
   while the writer is still a zombie, both file-op markers remain clear,
   recovery close checkpoints retained non-marker WAL, and the next
   ownerless/native reopen still sees the pre-savepoint row.
   Deadlock victims after local explicit-transaction writes now reuse that
   discard rule after MyLite's internal deadlock rollback, while accepted 1205
   timeout victims prove the same discard after explicit rollback; focused
   two-process SQL coverage proves the victim process clears its process-local
   file-op redo latch while the winning transaction can still commit and a
   later no-live ownerless close after both children are reaped drains
   committed DML marker/WAL evidence after native checkpoint proof.
   That closes the focused checkpointed representative DML commit marker and
   rollback/deadlock/savepoint classification gaps, not the broader DML-origin
   `FILE_MODIFY`, crash, killed-transaction, or
   concurrent-writer explicit-transaction matrices.
   The no-argument
   aggregate harness remains
   available for manual runs, while CTest registers the normal ownerless SQL
   coverage as sixteen deterministic weighted shards under the same
   `compat.ownerless-cross-process-sql` label so long aggregate runs expose
   per-shard estimated weight, timing, failure identity, and flushed
   active-case name/index diagnostics on timeout. The old modulo `sql-shard`
   command remains available for comparison. Hidden per-case children run in their own process
   groups so timeout cleanup cannot leave orphaned descendants holding CTest
   output pipes open, and the per-case timeout now uses a monotonic wall-clock
   deadline so scheduler delays cannot stretch the nominal timeout window. The
   runner also exposes `sql-case <index-or-name>` to rerun a named timeout
   through the same hidden-child wrapper without inventing a one-case shard.
   Attempted two-job and four-job preset-level modulo-shard scheduling exposed
   load-sensitive ownerless DDL/dictionary/temporary-tablespace timeouts.
   Weighted-shard ownerless SQL measurement now passes at two jobs locally with
   about half the serial ownerless SQL wall time. Full embedded preset
   two-job scheduling still timed out when ownerless SQL interleaved with
   unrelated embedded tests. A later CI visibility refresh keeps non-ownerless
   tests and ownerless SQL as separate visible steps, but runs each ownerless
   SQL case through the direct `sql-case <index>` harness path with `/tmp`
   ownerless cleanup between cases. Paired ownerless shard runs timed out in
   shard `.0` at `test_ownerless_index_idempotent_ddl_refreshes_peer_dictionary`
   and then in shard `.14` at
   `test_ownerless_view_prepared_dml_enforces_check_option`, even though direct
   reruns of the timed-out case class passed quickly. A one-shot serial
   full-label run later timed out in shard `.9` at
   `test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`, while
   the isolated shard passed. An isolated shard `.3` run then timed out at
   `test_ownerless_foreign_key_child_rename_refreshes_peer_dictionary`, while
   the direct case passed.
   Shard `.11` was therefore marked `RUN_SERIAL` for the FK
   cross-schema child-rename case. CI run `27864786059` later timed out shard
   `.8` at `test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`;
   the exact direct case passed locally in about 3 seconds, the exact
   `sql-weighted-shard 8 16` CTest shard passed locally in under 30 seconds,
   and the failed CI job passed on rerun. Shard `.8` now also runs with
   `RUN_SERIAL`, preserving coverage while avoiding the documented
   load-sensitive DDL/dictionary timeout pattern.
   The aggregate harness now
   execs both hidden test-case children and the exclusive initializer so worker
   processes do not inherit post-runtime global state. The preset also
   runs explicit multi-statement
   transaction
   stress with
   `MYLITE_OWNERLESS_TX_STRESS_ROUNDS=80`, covering concurrent independent-table
   transactions, savepoint rollback inside every transaction, final aggregate
   oracles, and forced `.shm` rebuild plus native exclusive reopen after the
   workers finish. The `ownerless-transaction-stress-trace-export` slice adds
   `tools/ownerless-transaction-stress-trace`, which emits schema, per-worker
   SQL, an expected aggregate/rollback oracle, and a manifest for external
   MariaDB/RQG-style runners using the same deterministic transaction/savepoint
   schedule. The `ownerless-external-trace-runner` slice adds
   `tools/ownerless-sql-trace-runner`, which validates generated trace
   packages without an external server and can replay schema, concurrent
   worker/reader SQL, post-worker SQL, expected-error probes, and final oracle
   SQL through a user-supplied MariaDB-compatible client. The
   `ownerless-sql-trace-suite` slice adds
   `tools/ownerless-sql-trace-suite`, which generates every deterministic
   ownerless trace family, validates each package through the common runner,
   and can pass a user-supplied MariaDB-compatible client through for external
   replay. The `ownerless-external-mariadb-trace-smoke` slice adds
   `tools/ownerless-external-mariadb-trace-smoke`, an opt-in Docker-backed
   path that starts a disposable MariaDB container and runs the deterministic
   trace suite through a real external `mariadb` client while default CMake
   coverage only validates the dependency-free command plan. The FK graph trace
   now emits bounded stored-procedure retry loops for ordinary MariaDB
   `1205`/`1213` contention and SQLSTATE `40001` deadlock reporting, so the
   Docker smoke can replay the deterministic FK graph with the rest of the
   suite; `--skip-trace` remains available for constrained environments. It also
   runs
   active-reader pressure stress with
   `MYLITE_OWNERLESS_ACTIVE_READER_PRESSURE_ROUNDS=48`, holding a
   repeatable-read snapshot pin across repeated writer opens before forced
   `.shm` rebuild and native exclusive reopen checks. Expanding-page pressure
   stress runs the same active-reader shape over distinct large rows with
   `MYLITE_OWNERLESS_EXPANDING_PAGE_PRESSURE_ROWS=48`. BLOB page pressure
   stress runs the same active-reader shape over `ROW_FORMAT=DYNAMIC`
   off-page `LONGBLOB` payloads with
   `MYLITE_OWNERLESS_BLOB_PAGE_PRESSURE_ROWS=12`. Compressed BLOB page
   pressure stress runs the same shape over `ROW_FORMAT=COMPRESSED
   KEY_BLOCK_SIZE=8` off-page `LONGBLOB` payloads with
   `MYLITE_OWNERLESS_COMPRESSED_BLOB_PAGE_PRESSURE_ROWS=8`. The
   compressed BLOB key-block matrix covers the same active-reader retention and
   post-release checkpoint lifecycle for `KEY_BLOCK_SIZE=1`, `2`, `4`, `8`, and
   `16`, with native `ZBLOB`/`ZBLOB2` page evidence for each table. The
   `ownerless-active-reader-pressure-trace-export` slice adds
   `tools/ownerless-active-reader-pressure-trace`, which emits a
   repeatable-read snapshot reader, a deterministic large-row writer schedule,
   replacement-copy DDL worker input with copied-metadata oracles,
   AUTO_INCREMENT high-watermark ALTER worker input with implicit-ID oracles,
   expected aggregate/version/payload oracle SQL, and a manifest for external
   MariaDB/RQG-style runners. Its worker uses a bounded MariaDB `1205`/`1213`
   plus SQLSTATE `40001` retry procedure, and its reader uses a bounded
   MariaDB `1020`/`1205`/`1213` plus SQLSTATE `40001` retry procedure, so
   raw-client external replay does not abort on ordinary snapshot/writer
   contention before the final aggregate oracle runs. The
   `ownerless-blob-pressure-trace-export` slice adds
   `tools/ownerless-blob-pressure-trace`, which emits retry-aware deterministic
   dynamic and compressed BLOB pressure SQL with snapshot and final aggregate
   oracles for the same external trace-runner contract. The
   `ownerless-ctas-dml-trace-export` slice adds
   `tools/ownerless-ctas-dml-trace`, which emits deterministic CTAS
   create/update/delete/insert SQL, repeatable-read snapshot reader polls, and
   final table/column/aggregate oracles for external trace-runner input. It
   joins the dependency-free check-mode trace suite, and focused Docker-backed
   MariaDB 11.8 replay of `--trace ctas-dml --scale 2` passed with
   `trace_count=1`, `suite_run=ok`, and `external_mariadb_trace_smoke=ok`. The
   `ownerless-compressed-row-format-trace-export` slice adds
   `tools/ownerless-compressed-row-format-trace`, which emits deterministic
   external trace-runner input for bounded `ROW_FORMAT=COMPRESSED
   KEY_BLOCK_SIZE` rebuild cycles, retry-aware repeatable-read reader polling,
   per-round compressed metadata checks, final aggregate/metadata oracles, and
   a manifest. It joins the dependency-free check-mode trace suite as the
   twelfth trace family. Focused Docker-backed MariaDB 11.8 replay of
   `--trace compressed-row-format-ddl --scale 2` passed with `trace_count=1`,
   `suite_run=ok`, and `external_mariadb_trace_smoke=ok`. The
   `ownerless-pressure-external-replay-evidence` slice adds a dependency-free
   scaled CTest check for the active-reader and BLOB pressure traces and records
   a Docker-backed MariaDB 11.8 replay of `--trace active-reader-pressure
   --trace blob-pressure --scale 2`, which passed both final oracles with
   `external_mariadb_trace_smoke=ok`. The
   `ownerless-pressure-scale3-replay` follow-up adds a second dependency-free
   pressure check at scale `3` and records Docker-backed MariaDB 11.8 replay of
   the same active-reader and BLOB pressure trace subset at scale `3`, including
   active-reader replacement-copy and AUTO_INCREMENT high-watermark oracles plus
   dynamic and compressed BLOB final oracles. After BLOB pressure reader/worker retry
   hardening and FK graph SQLSTATE `40001` retry hardening, the full scale-1
   Docker-backed MariaDB replay passed all 10 deterministic traces with
   `trace_count=10` and `external_mariadb_trace_smoke=ok`. The
   `ownerless-external-full-scale2-replay` slice then hardens active-reader
   reader retries and records full scale-2 Docker-backed MariaDB replay of all
   10 deterministic traces with `trace_count=10`, `suite_run=ok`, and
   `external_mariadb_trace_smoke=ok`. That replay is historical evidence for the
   10 trace families present before CTAS DML export raised the check-mode suite
   to 11 families; focused CTAS DML external replay is recorded separately. The
   `ownerless-external-full11-scale2-replay` slice then records full scale-2
   Docker-backed MariaDB replay of all 11 deterministic traces with
   `trace_count=11`, `suite_run=ok`, and `external_mariadb_trace_smoke=ok`.
   A current-suite rerun after adding the active-reader AUTO_INCREMENT
   high-watermark oracle also passed all 11 scale-2 traces and verified the
   active-reader final AUTO_INCREMENT state `rows=4`, `sum(id)=106`,
   `max(id)=100`, and `sum(value)=1060`. The
   `ownerless-external-full12-scale2-replay` slice then hardens CTAS DML reader
   retries after raw-client MariaDB returned `ERROR 1020` under concurrent
   CTAS/DML replay, and records full scale-2 Docker-backed MariaDB replay of
   all 12 deterministic traces with `trace_count=12`, `suite_run=ok`, and
   `external_mariadb_trace_smoke=ok`, including the compressed row-format DDL
   trace.
   Normal ownerless SQL coverage also verifies
   no-live close-time reclaim after a raw-latest versus page-visible checkpoint
   gap, the opt-in active-reader pressure limit for direct/prepared writes
   including prepared `INSERT ... SELECT`,
   representative DML/DDL write classes, variant DML/index/rename/truncate
   spellings, DML modifier spellings including `INSERT IGNORE`,
   `UPDATE LOW_PRIORITY`, and `DELETE LOW_PRIORITY QUICK`, CTAS post-create
   `UPDATE`/`DELETE`/`INSERT ... SELECT` pressure coverage, column ALTER
   pressure variants for `MODIFY COLUMN`,
   `CHANGE COLUMN`, `DROP COLUMN`, `RENAME COLUMN`, and
   `ALTER COLUMN ... SET/DROP DEFAULT`,
   CHECK and FOREIGN KEY constraint add/drop pressure variants,
   storage/rebuild pressure variants for charset conversion, `FORCE`, ordinary
   row-format ALTER, and compressed `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`,
   `2`, `4`, `8`, and `16` ALTERs, `ALTER TABLE ... AUTO_INCREMENT`
   high-watermark pressure coverage, generated-column ALTER and generated-column secondary
   index add/drop pressure variants, generated-column FK add/drop pressure
   variants,
   schema/table-copy/replacement/replacement-copy/view/trigger dictionary
   variants, and DDL variant spellings for schema alteration, table and trigger
   idempotent no-ops, view replacement/alteration, trigger replacement, and
   explicit policy-before-pressure rejection for representative
   process-control, account/grant, plugin, binlog, logging, query-cache,
   event/scheduler, host-file import, table-admin, locked-table, flush-lock,
   tablespace, partitioned-table DDL, storage-option, and sequence SQL,
   local post-DDL conservative-write coverage for rename-away plus same-name
   recreate under an active retained page-version pin,
   plus the
   public active-pin/WAL pressure diagnostic.
   Each stress test has a 900-second timeout. Long-running randomized external
   MariaDB/RQG oracle execution remains environment-owned follow-up work, but the
   deterministic trace-suite and external-MariaDB smoke bridges now provide
   reproducible generated-input and real-client replay entry points, including
   bounded `--scale` profiles, focused `--trace` subsets, and multi-seed random
   transaction, DDL stress, and FK graph generated traces for deterministic
   external stress probes, including focused Docker-backed replay for the
   default DDL seed set and contiguous combined random transaction plus DDL
   seed-sweep replay windows through seed `1279`; the `16` through `31`,
   `32` through `63`, `64` through `95`, `96` through `127`, and `128`
   through `159`, `160` through `191`, `192` through `223`, and `224` through
   `255`, `256` through `287`, `288` through `319`, `320` through `351`,
   `352` through `383`, `384` through `415`, `416` through `447`, `448`
   through `479`, `480` through `511`, `512` through `543`, `544` through
   `575`, `576` through `607`, `608` through `639`, `640` through `671`,
   `672` through `703`, `704` through `735`, `736` through `767`,
   `768` through `799`, `800` through `831`, `832` through `863`,
   `864` through `895`, `896` through `927`, `928` through `959`,
   `960` through `991`, `992` through `1023`, `1024` through `1055`,
   `1056` through `1087`, `1088` through `1119`, `1120` through `1151`,
   `1152` through `1183`, `1184` through `1215`, `1216` through `1247`, and
   `1248` through `1279`
   replays also cover FK graph seeds at rounds `2`. FK
   graph now
   participates in the
   dependency-free combined seed-sweep command-plan windows through seed `1279`,
   and focused Docker-backed FK graph seed
   replay has passed the default seed set plus the `16` through `31`, `32`
   through `63`, `64` through `95`, `96` through `127`, `128` through `159`,
   `160` through `191`, `192` through `223`, `224` through `255`, and `256`
   through `287`, `288` through `319`, `320` through `351`, `352` through
   `383`, `384` through `415`, `416` through `447`, `448` through `479`,
   `480` through `511`, `512` through `543`, `544` through `575`, `576`
   through `607`, `608` through `639`, `640` through `671`, `672` through
   `703`, `704` through `735`, `736` through `767`, `768` through `799`,
   `800` through `831`, `832` through `863`, `864` through `895`,
   `896` through `927`, `928` through `959`, `960` through `991`,
   `992` through `1023`, `1024` through `1055`, `1056` through `1087`,
   `1088` through `1119`, `1120` through `1151`, `1152` through `1183`,
   `1184` through `1215`, `1216` through `1247`, and `1248` through `1279`
   windows with bounded whole-seed retries for transient raw MariaDB deadlock
   exits.

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
  - ownerless runtime startup serialization across native startup, connection,
    core `mysql.*` bootstrap, dictionary-generation initialization, final
    no-live ownerless native DDL file-operation checkpoint evidence, and final
    ownerless native shutdown redo-header repair using
    `concurrency/mylite-runtime-startup.lock`, with bounded retry after partial
    MariaDB embedded startup cleanup and redo-prefix restore, plus ordinary
    native read/write reopen recovery after ownerless DDL-policy handoffs when
    durable ownerless redo evidence exists without retained page WAL, and
    ordinary exclusive repeated-open redo-prefix repair when final embedded
    shutdown leaves an invalid startup prefix, while active-runtime reconnect
    timing remains measured separately from full startup/shutdown cost, and
    hook-only validation that malformed saved redo-header backups do not arm
    the ordinary-open recovery bridge,
  - opener crash,
  - `.shm` creation, validation, rebuild, resize, and remap,
  - incompatible `.shm` format rejection,
  - stale copied `.shm` rebuild after closed-directory copy, now covered by
    header file-identity validation,
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
  - gap locks; SQL coverage proves a secondary-index next-key/gap lock blocks
    a peer insert and that a fresh ownerless peer can insert the same key after
    the holder rolls back,
  - foreign keys, including ownerless peer-visible `ON UPDATE CASCADE`,
    `ON DELETE CASCADE`, `ON DELETE SET NULL`, `ON DELETE RESTRICT`, and
    composite/deep/generated-column policy/cyclic foreign-key coverage plus
    same-schema and cross-schema parent/child rename refresh plus same-schema
    and cross-schema multi-pair parent/child rename refresh,
  - rollback and savepoints; SQL coverage proves savepoint rollback remains
    invisible to a peer before commit and the surviving update becomes visible
    after commit, with stress/export coverage for broader savepoint schedules.
- page visibility:
  - committed data visible in another process,
  - uncommitted data invisible,
  - long reader with writer and checkpoint; primitive coverage proves a
    checkpoint writer waits behind an active cross-process page-log reader,
  - live idle peer with checkpoint reclamation; SQL coverage proves close-time
    reclamation can checkpoint native-support-only page-version WAL while a
    peer is open with no active page-version pin or native write/recovery state,
    and live-writer coverage proves native checkpoint reclamation is skipped while shared
    explicit-transaction, transaction/redo/lock write state remains active,
  - live snapshot pin with checkpoint reclamation; SQL coverage proves a
    repeatable-read snapshot blocks live-peer prefix compaction until release,
    and killed pinned-reader coverage proves dead-owner cleanup releases the
    reader's MDL, read-view, and pin state so a later live-peer close can
    reclaim; primitive and unsafe-hook coverage keep required boundary
    retention and multi-pin newer-record retention separate from
    single-snapshot post-snapshot compaction, while product close-time reclaim
    retains WAL whenever an active pin remains and no-live close forces native
    checkpoint proof before truncating retained WAL after the pin releases,
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
    waiter's shared table-wait entry. Hook-build SQL coverage proves a native
    `foreign_key_checks=0` and `unique_checks=0` empty-table bulk insert
    waiting behind a peer ownerless `LOCK IN SHARE MODE` reader publishes a
    shared external native table-wait registry entry, clears it after release,
    and remains durable through ownerless/native reopen and forced `.shm`
    rebuild. Embedded hook coverage directly asserts retained external
    table-wait snapshots dispatch through `wait_until_table` with stable
    transaction ID, table ID, mode, timeout, and result propagation.
    Hook-build SQL crash coverage kills that same native table-wait
    SQL waiter after the shared table-wait entry is published, verifies the
    dead wait remains observable, verifies live-peer cleanup remains busy while
    the blocking reader is alive, verifies no-live recovery removes the dead
    wait after the blocker dies, confirms the interrupted insert is absent, and
    retries the insert through ownerless/native reopen and forced `.shm`
    rebuild. Hook-build SQL negative proof arms the
    local ownerless table-wait callback while representative blocked `ALTER TABLE`,
    instant add/rename column, column modify/default, table comment,
    CHECK/FK add, `CREATE INDEX`, unique and online index add, existing-index
    drop/rename/ignored, copy-force and primary-key replacement `ALTER TABLE`,
    charset conversion, row-format ALTER, `TRUNCATE TABLE`, `RENAME TABLE`,
    `DROP TABLE`, `CREATE OR REPLACE TABLE ... LIKE`, and
    `CREATE OR REPLACE TABLE ... AS SELECT` variants time out, verify blocked
    metadata remains unchanged, and fail if any tested SQL shape reaches the
    local callback, so positive SQL-level local table-wait fault injection
    remains unclaimed beyond the covered external native table-wait registry
    path.
    Ownerless SQL `LOCK TABLES`/`UNLOCK TABLES` is rejected until SQL locked-table
    mode has a design,
  - before/after page-version append,
    before-append page-version SQL hook coverage proves recovery remains
    readable and later ownerless writes can proceed after a writer is killed
    before appending a page-version WAL record,
  - after redo bytes are marked written but before latest-checkpoint publish,
  - after volatile page-visible publish but before durable checkpoint,
  - after plain non-temporary `CREATE TABLE` native file-per-table creation
    reaches ownerless dictionary prefinish; hook coverage proves a live peer
    can clean up the dead creator, finish the marked dictionary generation, see
    the created `.frm`/`.ibd`, insert rows, and keep the native file-op marker
    durable until the final no-live checkpoint drain,
  - after standalone secondary-index creation/removal, secondary-index rename,
    and secondary-index ignored/not-ignored metadata changes but before
    ownerless dictionary finish; hook coverage proves live-peer cleanup remains
    busy until no-live recovery and the recovered present/absent, renamed, and
    ignored/not-ignored index states remain correct,
  - after ordinary column-add, column-drop, column-modify, and column-rename
    ALTER but before ownerless dictionary finish; hook coverage proves live-peer
    cleanup remains busy until no-live recovery and the recovered
    added/default, absent-column, modified-column, renamed-column, or
    dependent-expression rename state remains correct,
  - after duplicate `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing
    `ALTER TABLE ... DROP COLUMN IF EXISTS` no-op success but before ownerless
    dictionary finish; hook coverage proves live-peer cleanup remains busy until
    no-live recovery and preserved original column/default metadata,
    missing-column absence, MariaDB 1060/1091 retry errno, post-recovery writes,
    ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after missing `ALTER TABLE ... MODIFY COLUMN IF EXISTS`,
    `ALTER TABLE ... RENAME COLUMN IF EXISTS`,
    `ALTER TABLE ... CHANGE COLUMN IF EXISTS`,
    `ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and
    `ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` no-op success but
    before ownerless dictionary finish; hook coverage proves live-peer cleanup
    remains busy until no-live recovery and preserved original column/default
    metadata, missing and attempted renamed/changed-column absence,
    MariaDB 1054 retry errno, post-recovery writes, ownerless/native reopen,
    and forced `.shm` rebuild remain correct; focused generated-column/CHECK
    expression-table missing rename, change, and default no-ops also preserve
    generated values, real defaults, and CHECK enforcement,
  - after representative `ALTER TABLE ... AUTO_INCREMENT` native
    high-watermark persistence but before ownerless dictionary finish; hook
    coverage proves live-peer cleanup remains busy until no-live recovery,
    recovered implicit ID allocation remains monotonic, ownerless/native reopen
    works, and forced `.shm` rebuild remains correct,
  - after representative composite direction primary-key replacement native
    clustered-key rebuild but before ownerless dictionary finish; hook coverage
    proves live-peer cleanup remains busy until no-live recovery, recovered
    key-part direction metadata, replacement-key enforcement, old-key duplicate
    writes, ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after representative `ALTER COLUMN ... SET DEFAULT` native metadata update
    but before ownerless dictionary finish; hook coverage proves live-peer
    cleanup remains busy until no-live recovery and recovered default metadata,
    post-recovery default-backed inserts, ownerless/native reopen, and forced
    `.shm` rebuild remain correct,
  - after representative `ALTER TABLE ... COMMENT` native metadata update but
    before ownerless dictionary finish; hook coverage proves live-peer cleanup
    remains busy until no-live recovery and recovered comment metadata,
    retained rows, post-recovery DML, ownerless/native reopen, and forced
    `.shm` rebuild remain correct,
  - after representative `ALTER TABLE ... CONVERT TO CHARACTER SET` native
    metadata/storage update but before ownerless dictionary finish; hook
    coverage proves live-peer cleanup remains busy until no-live recovery and
    recovered charset/collation metadata, retained rows, post-recovery DML,
    ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after representative `CREATE TABLE ... LIKE` destination table creation
    but before ownerless dictionary finish; hook coverage proves live-peer
    cleanup can finish the dead dictionary generation, recovered native files,
    table/column metadata, copied `LIKE` secondary-index metadata,
    post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild
    remain correct, and the native file-op marker drains only after final
    no-live close,
  - after representative `CREATE TABLE ... SELECT` destination table creation
    and population but before ownerless dictionary finish; hook coverage proves
    live-peer cleanup can finish the dead dictionary generation, recovered
    native files, table/column metadata, CTAS copied rows, post-recovery writes,
    ownerless/native reopen, and forced `.shm` rebuild remain correct, and the
    native file-op marker drains only after final no-live close,
  - after representative `CREATE OR REPLACE TABLE` native old-table
    replacement but before ownerless dictionary finish; hook coverage proves
    live-peer cleanup remains busy until no-live recovery and recovered
    replacement native files, old-column/index absence, new-column/index
    metadata, empty replacement rowset, post-recovery writes, ownerless/native
    reopen, and forced `.shm` rebuild remain correct,
  - after representative `CREATE OR REPLACE TABLE ... LIKE` and
    `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy completion but
    before ownerless dictionary finish; hook coverage proves live-peer cleanup
    remains busy until no-live recovery and recovered replacement files,
    old-column/index absence, copied `LIKE` secondary-index metadata, CTAS
    copied rows, post-recovery writes, ownerless/native reopen, and forced
    `.shm` rebuild remain correct,
  - after duplicate `CREATE TABLE IF NOT EXISTS` and missing
    `DROP TABLE IF EXISTS` no-op success but before ownerless dictionary
    finish; hook coverage proves live-peer cleanup remains busy until no-live
    recovery and preserved native table metadata, missing-table absence,
    ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after duplicate top-level `CREATE INDEX IF NOT EXISTS` and missing
    top-level `DROP INDEX IF EXISTS` no-op success but before ownerless
    dictionary finish; hook coverage proves live-peer cleanup remains busy until
    no-live recovery and preserved index key-part metadata, missing-index
    absence, ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` and missing
    `ALTER TABLE ... DROP INDEX IF EXISTS` no-op success but before ownerless
    dictionary finish; hook coverage proves live-peer cleanup remains busy until
    no-live recovery and preserved ALTER-index key-part metadata, missing-index
    absence, post-recovery writes, ownerless/native reopen, and forced `.shm`
    rebuild remain correct,
  - after duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS` and duplicate
    `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op success but before
    ownerless dictionary finish; hook coverage proves live-peer cleanup remains
    busy until no-live recovery and preserved unique key-part metadata,
    duplicate-key enforcement, attempted-key non-enforcement, ownerless/native
    reopen, and forced `.shm` rebuild remain correct,
  - after representative `CREATE DATABASE` native schema directory/`db.opt`
    creation but before ownerless dictionary finish; hook coverage proves
    live-peer cleanup remains busy until no-live recovery and recovered schema
    defaults, post-recovery table writes, ownerless/native reopen, and forced
    `.shm` rebuild remain correct,
  - after representative `ALTER DATABASE` native `db.opt` rewrite but before
    ownerless dictionary finish; hook coverage proves live-peer cleanup remains
    busy until no-live recovery and recovered schema defaults, pre-alter table
    collation preservation, post-recovery table default inheritance,
    ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after duplicate `CREATE DATABASE IF NOT EXISTS` and missing
    `DROP SCHEMA IF EXISTS` no-op success but before ownerless dictionary
    finish; hook coverage proves live-peer cleanup remains busy until no-live
    recovery and preserved schema defaults, preserved real schema/table state,
    missing-schema absence, ownerless/native reopen, and forced `.shm` rebuild
    remain correct,
  - after failed generated-column CREATE/ALTER/primary-key validation but
    before ownerless dictionary finish; hook coverage proves live-peer cleanup
    remains busy until no-live recovery, rejected tables/columns/files remain
    absent, and MariaDB retry errno 1901/1903 remains stable,
  - after successful generated-column CREATE TABLE, generated-column
    ALTER TABLE ... ADD COLUMN, and generated-column secondary-index creation
    but before ownerless dictionary finish; hook coverage proves live-peer
    cleanup remains busy until no-live recovery and recovered generated-column
    metadata, generated values, forced generated-column index reads,
    ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after generated-column child and referenced-column
    `ALTER TABLE ... ADD CONSTRAINT` FK creation but before ownerless
    dictionary finish; hook coverage proves live-peer cleanup remains busy
    until no-live recovery and recovered generated-column values, FK metadata,
    missing-parent errors, restricted-update errors, cascaded deletes,
    ownerless/native reopen, and forced `.shm` rebuild remain correct,
  - after generated-column child and referenced-column
    `ALTER TABLE ... DROP FOREIGN KEY` removal but before ownerless dictionary
    finish; hook coverage proves live-peer cleanup remains busy until no-live
    recovery and recovered generated-column values, absent FK metadata, orphan
    writes, parent deletes without stale cascade/restrict behavior,
    ownerless/native reopen, and forced `.shm` rebuild remain correct,
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
  - database-directory primitive probe coverage,
  - hook-only ownerless open rejection for failed directory probes,
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
is allowed because the next read/write opener validates the copied `.shm` file
identity and discards/rebuilds volatile segments when it differs; copying an
open directory is unsupported until an ownerless backup protocol coordinates
reader slots, checkpoints, and page-version retention.

Compatibility status should stay partial until at least Phase 9 passes. Shared
read-only opens can be claimed for the tested SQL policy and committed-read
visibility surface, including prepared `SELECT` execution, read-only
transaction first-read/repeatable-snapshot behavior, reads inside transactions
after local writes, and no-live-process page-version replay; true
InnoDB `innodb_read_only` startup, ownerless cross-process dirty reads, and full
live-peer DDL/file-lifecycle tablespace crash recovery remain planned.
Ownerless read/write prepared plain reads now also share the one-shot stale
InnoDB dictionary-cache `1932` recovery path with direct plain reads for the
tested trigger DDL peer-created tablespace case.
Ownerless no-result prepared `INSERT`, `UPDATE`, `DELETE`, and `REPLACE`
now avoid native MariaDB `MYSQL_STMT` creation in ownerless read/write mode.
MyLite counts `?` parameter markers for that subset with its SQL tokenizer at
public `mylite_prepare()` time, then renders bound values into SQL literals and
executes the statement through MariaDB's length-aware text query path inside
the protected `mylite_step()` execution boundary. This keeps native InnoDB
prepared-DML table and dictionary state from remaining live across
independent-process readiness or wait barriers without blocking peer joins on a
native statement-cache lease; prepared reads and result-returning DML keep the
existing native prepare path. Current
product no-live replay skips retained page-version records for tablespaces no
longer present during dirty recovery, no-live final ownerless close publishes
native checkpoint evidence for completed DDL file operations before shutdown,
no-live stale-reader rebuilds checkpoint retained reader-boundary WAL before
segment rebuild with focused dropped, same-schema and cross-schema
same-statement multi-dropped, ordinary-created, LIKE-copy, CTAS-created with
post-create DML, recreated, rename-away plus new original-name create,
`CREATE OR REPLACE TABLE` replacement including LIKE and CTAS replacements,
renamed, truncated, force-rebuilt, primary-key-rebuilt, and compressed
row-format-rebuilt file-per-table SQL coverage, multi-rename swap coverage, plus
multi-table schema-drop absence, and
local post-DDL insert coverage that keeps visible write fast paths disabled
while an external page-version pin is active after dictionary DDL and
suppresses native snapshot-boundary synthesis plus external space-allocation
refresh for those local post-DDL writes,
and
hook-build coverage now kills same-schema,
cross-schema, same-schema multi-pair swap, and cross-schema multi-pair swap
`RENAME TABLE` writers after the
native file move but before ownerless dictionary finish, plus marker-specific
coverage for the same `RENAME TABLE` boundary, a `CREATE TABLE ... LIKE`
writer after native `FILE_CREATE`, a CTAS writer after native `FILE_CREATE`
and row population, a `TRUNCATE TABLE` writer after native truncate/recreate,
and a `DROP TABLE` writer after native `FILE_DELETE`, replacement-copy
`CREATE OR REPLACE TABLE ... LIKE` and `CREATE OR REPLACE TABLE ... AS SELECT`
writers after native replacement-copy completion, marker-specific coverage for
representative `ALTER TABLE ... FORCE` and
`ALTER TABLE ... ROW_FORMAT=DYNAMIC` rebuild writers after native rebuild
completion, focused post-checkpoint DML observation of the native
`FILE_MODIFY` redo flag, an `ALTER TABLE ... FORCE, ALGORITHM=COPY` writer
after native table-copy rebuild, a `CREATE OR REPLACE TABLE` writer
after native old-table replacement, duplicate `CREATE TABLE IF NOT EXISTS` and
missing
`DROP TABLE IF EXISTS` no-op writers, duplicate top-level
`CREATE INDEX IF NOT EXISTS`, missing top-level `DROP INDEX IF EXISTS`,
duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS`, and missing
`ALTER TABLE ... DROP INDEX IF EXISTS` no-op writers, duplicate top-level
`CREATE UNIQUE INDEX IF NOT EXISTS` and duplicate
`ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op writers, duplicate
`CREATE DATABASE IF NOT EXISTS` and missing `DROP SCHEMA IF EXISTS` no-op
writers, secondary-index rename and
ignored/not-ignored metadata writers after native index metadata changes, a
primary-key replacement writer after native clustered-key rebuild, foreign-key
ADD/DROP writers after native
constraint metadata creation/removal, CHECK ADD/DROP writers after native
table-definition mutation, a cross-schema foreign-key multi-pair rename writer
after native FK metadata rewrite and file movement, a same-schema
parent-through-temporary foreign-key multi-pair rename writer after native FK
metadata rewrite and file movement, a CHECK ADD variant that combines
column-level CHECK metadata with a generated-column CHECK expression, missing
`ALTER TABLE ... MODIFY COLUMN IF EXISTS`,
missing `ALTER TABLE ... RENAME COLUMN IF EXISTS`, missing
`ALTER TABLE ... CHANGE COLUMN IF EXISTS`, missing
`ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and missing
`ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` no-op writers after
MariaDB success, including generated-column/CHECK expression-table missing
rename, change, and default no-ops, an `ALTER TABLE ... AUTO_INCREMENT` writer after
native high-watermark persistence, a composite direction primary-key writer
after native clustered-key rebuild, an `ALTER COLUMN ... SET DEFAULT` writer
after native metadata update, simple view CREATE/DROP writers after native view
definition-file creation/removal, simple trigger CREATE/DROP, trigger
replacement, ordered trigger PRECEDES, duplicate `CREATE TRIGGER IF NOT EXISTS`,
missing `DROP TRIGGER IF EXISTS`, delayed missing-dependency `CREATE TRIGGER`,
and explicit `CREATE DEFINER=CURRENT_USER TRIGGER` writers after native
`.TRG`/`.TRN` metadata creation/removal, rewrite, no-op preservation, delayed
dependency acceptance, or definer metadata storage, charset-conversion, dynamic
row-format, compressed 4 KiB/8 KiB/16 KiB row-format, and table-comment writers
after native table-option metadata update or rebuild, a
`DROP TABLE` writer after native file removal, a stale-reader retained-WAL
`DROP TABLE` writer after native file removal before ownerless dictionary
finish, and a `DROP DATABASE` writer
after native schema/table removal plus a `CREATE DATABASE` writer after native
schema directory/`db.opt` creation and an `ALTER DATABASE` writer after native
`db.opt` rewrite but before ownerless dictionary finish, and
successful generated-column CREATE TABLE, generated-column ALTER TABLE ...
ADD COLUMN, generated-column secondary-index, and generated-column
child/referenced-column FK ADD/DROP writers after native metadata completion or
removal but before ownerless dictionary finish, and verifies no-live
ownerless/native reopen of the recovered table, generated-column, index,
foreign-key, or schema states, including recovered old/new index-name and
ignored/not-ignored metadata, but MyLite still lacks durable file lifecycle
metadata for broader DDL recovery.

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
- Long readers can starve checkpoint progress, as in SQLite WAL. A configured
  ownerless page-log limit can throttle new writes with `MYLITE_BUSY`,
  `mylite_ownerless_pressure_status()` exposes the current pin/WAL pressure
  state, and thresholded ownerless write/DDL/transaction-end
  statement-boundary scheduling can reclaim user-page WAL when no peer process
  is live; with idle live peers it can reclaim native-support-only WAL after
  the statement gate proves no active page-version pins or native
  write/recovery state, while user WAL remains retained. A runtime-owned timer
  scheduler can reclaim after reader pins release while an ownerless writer
  remains open and idle,
  without waiting for another SQL statement or close-time cleanup. Foreground
  statement reclaim uses a larger internal WAL budget while a runtime remains
  in its single-owner epoch, so tight single-process write bursts rely on timer
  or close cleanup below that budget without changing peer-seen scheduling or
  native marker durability. Pending native file-operation checkpoint markers no
  longer force synchronous foreground reclaim below that budget; they remain
  durable and are still drained by timer, no-live, or close-time reclaim. The
  statement scheduler checks that single-owner foreground budget before taking
  the active page-version pin snapshot, and the production insert performance
  probe drains benchmark setup DDL through close/reopen before timing ownerless
  insert loops so CI separates steady-state DML cost from setup checkpoint
  cleanup.
  The same single-owner proof skips non-forced page-write, space-metadata, and
  explicit-transaction buffer-pool first-write refresh only while the owner
  generation still matches, no peer process is live, no peer-owned snapshot
  page-version pin is active, and a redo/checkpoint baseline exists;
  owner-local direct-read pins do not disable that single-owner refresh skip,
  but they do block foreground/timer checkpoint scheduling while their oldest
  read LSN is below the current visible boundary because the shared handle pin
  can remain open after a successful direct read. Current-owner direct-read
  pins do not force the hot synthesized snapshot-boundary WAL scan path during
  same-runtime single-owner writes; stale-generation and peer-owned pins still
  do, because they cannot rely on this process's current native InnoDB MVCC
  state. The local-native current-read gate remains conservative once the
  runtime has consumed page-version WAL, because that path cannot yet prove all
  consumed pages are durably native-visible. Page-version reads can still avoid
  repeated WAL scans in the narrower single-owner case: when the runtime proves
  one active statement, no active ownerless native write state, and no active
  ownerless transactions, the shared page index is trusted for absent
  materializable page images. Full native-support page images publish index
  entries; proof-only native-support metadata is never a readable page image.
  Direct indexed hits can skip appended-tail validation in the same
  single-owner one-statement epoch, including during the local visible-fast
  writer; absent-index proof remains disabled while active ownerless native
  write state is present. Retained snapshots, peer-present readers, startup-WAL
  readers, index scan-required states, and same-process multi-statement work
  keep the conservative WAL scan proof. The skip is still bypassed for
  process-generation changes and older handle-pin advancement, which are
  mandatory clean-page refresh boundaries rather than routine steady-state
  refresh checks. A no-live final close by a
  runtime that only consumed the current visible page-version WAL leaves that
  WAL for no-live recovery instead of truncating it without writer-owned native
  page evidence; reader-only consumers do not use newer native page LSNs as
  successor proof unless the retained payload matches exactly, and skip the
  external refresh side effect during that failed reclaim attempt. Read/write
  runtime shutdown with live peers, or when no-live
  status cannot be proven, now refreshes the local InnoDB buffer pool to the
  latest ownerless external LSN and waits for local dirty pages through the max
  of that ownerless LSN and the local native LSN before MariaDB embedded
  teardown, so a worker that exits after explicit-transaction writes cannot
  leave a stale process-local clustered page for `mysql_server_end()` to flush
  after peer commits. Proven no-live final close keeps the existing native
  checkpoint/reclaim path. A writer runtime cannot reclaim with live peers
  until it has consumed the current visible page-version WAL after its local
  writes.
  No-live writer reclaim still requires native page proof before truncating
  retained WAL. A newer native file-per-table page can prove a retained record
  only when the record carries the external-snapshot lineage marker from a
  runtime that consumed WAL retained for another owner's reader snapshot; plain
  concurrent-writer records, including commit-race records, still require exact
  native proof or replay. Explicit transaction-end statements with local
  writes serialize on the global ownerless write statement lock before
  current-state refresh
  except when a peer holding that global statement read lock is waiting on this
  transaction's shared InnoDB or page-write lock and the shared registry proves
  the blocker relationship;
  read-only transactions that only used native locking reads keep conservative
  active-transaction refresh behavior but do not block their `COMMIT`/full
  `ROLLBACK` behind a peer writer's global gate. Forced page-version refreshes
  and any peer/reader case keep the conservative refresh path. The production
  WordPress CI timing job now enables a Release-build guard so its
  `perf-probe` and test-only PHPUnit phases refuse stale non-production
  PHP-extension artifacts before reporting timings, and it requires the
  transient WordPress MyLite test database directory outside the repository
  worktree while printing the database parent filesystem type so branch/main
  comparisons do not silently move onto the build-artifact path. The
  dependency and database-prep steps repeat the same production guards before
  publishing timings so a stale local or CI cache cannot be mistaken for a
  production PHPUnit result. The same
  WordPress CI job disables the defensive static `wpdb` property scan and
  child-process profiling for process-isolated PHPUnit timing while still
  closing the global `wpdb` before each child. The remaining process-isolated
  shards now use reconnect-disabled parent policy only after focused filter
  evidence: production `Tests_Formatting_Emoji` evidence on the measured host
  changed from `41.816s` shell real with full static scanning to `31.599s` with
  static scan disabled, a same-session static-scan-disabled A/B measured
  `28.711s` shell real with child profiling disabled versus `30.354s` with
  child profiling enabled, and the former eager UI/filesystem filter later
  passed `22` tests with `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0`,
  reporting `28.749s` PHPUnit time and `0.009 ms` parent reconnect per child
  through the renamed workflow variable.
  The
  production performance probe now classifies page-version publish volume by
  page type,
  splits ownerless mini-transaction publish and commit-log phase time, and
  page-visible commits use initialized page-log append and sync helpers for the
  already-open runtime WAL, but broader append-volume reduction still needs
  either safe page-image batching/dirty-page handoff or stronger native
  redo/checkpoint reconciliation evidence. The first MTR phase sample shows
  page-version publication dominates direct publish subphase time, but total
  ownerless MTR commit-log time remains well below the full autocommit
  `mysql_stmt_execute()` interval. Follow-up SQL/InnoDB handler profiling shows
  ownerless autocommit insert execution is mostly split between the SQL
  one-phase commit path into InnoDB and `ha_innobase::write_row()`, with
  `innobase_commit_low()` dominating the measured commit boundary and row
  insert internals accounting for the next largest block. A deeper InnoDB
  autocommit profile then shows that 400 ownerless autocommit inserts spend
  about `778 ms` in `trx_commit_for_mysql()`, including about `750 ms` in
  `trx_t::write_serialisation_history()` and only about `27 ms` in
  `commit_in_memory()`. The explicit ownerless commit-visibility block is
  about `20 ms`, while `row_insert_for_mysql()` accounts for about `230 ms`,
  mostly under clustered optimistic B-tree insertion. MTR page-log append
  batching now keeps the WAL format and record completion order unchanged while
  beginning an append session lazily for the current InnoDB publish scan and
  releasing it before active-reader boundary scans. The reduced stats-enabled
  production sample cut page-log append `fstat` time from about `21.8 ms` to
  `2.6 ms`, but the same 400-row sample still spent about `120 ms` in page-log
  append and about `171 ms` in commit-MTR page publication. The next
  performance target is therefore ownerless page-version write volume and
  native-support page publication, followed by clustered row-insert overhead;
  post-commit visibility release is no longer the leading suspect for the
  PHPUnit autocommit gap. Page-publish identity profiling then showed the
  reduced 400-row ownerless autocommit sample contained 3203 page-version
  publishes but only 5 repeated `(space_id,page_no,visible_lsn)` fingerprints,
  all native-support undo or transaction-system pages. That rules out simple
  same-visible-LSN duplicate suppression as a meaningful next optimization.
  A follow-up production prototype skipped native-support page publication when
  the existing single-owner/no-pin proof was active. It cut the reduced
  stats-enabled sample to 400 page publishes, but failed throughput validation:
  one stats-enabled run inflated write-history timing, and a 2000-row stats-off
  ownerless autocommit run regressed to about `168 ops/s` versus the prior
  baseline around `295 ops/s`. Native-support page publication therefore stays
  conservative until broader redo/checkpoint reconciliation can prove that
  fewer published native-support page images remain correct and faster.
  Production probes now emit compact `mylite_perf_summary_*` and
  `wordpress_perf_summary_*` keys so CI and local branch/main audits can
  compare startup, active-runtime reconnect, read throughput, and write
  throughput without losing the detailed phase counters. The embedded
  `mylite_perf_summary_*` startup keys also summarize warm open/close and
  active-runtime reconnect subphases for open total, ownerless platform probe,
  runtime start, runtime connect, system-table checks, dictionary handoff,
  `mysql_server_init()`, close total, runtime release, ownerless reclaim, and
  `mysql_server_end()` shutdown, making per-process startup and shutdown cost
  visible separately from active in-process reconnect cost. The ownerless
  startup probe now emits a first-probe open/close sample before
  `concurrency/mylite-ownerless-platform.meta` exists, a second fresh
  same-device ownerless database sample after the process-local device proof is
  cached, then cached warm ownerless open/close after the original directory's
  proof file is available, keeping the one-time filesystem proof cost separate
  from recurring ownerless startup and cold runtime/InnoDB startup.
  A public API parity benchmark is now also available as
  `tools/mylite_public_open_close_bench`, using only `mylite_open()`,
  `mylite_exec()`, and `mylite_close()` over an InnoDB table so the same source
  can be compiled against refs that predate branch-only internal probes. A
  local warmed comparison of current `ownerless-concurrency`
  (`793a085c2c2d25b74610ae1df1f965569bfe4013`) against `origin/main`
  (`4760d5128096e4560bc62cc19f7066bc15ff07d8`) reported branch warm
  open/close at `141.430-144.442 ms` for two 20-iteration samples, while main
  reported `346.062-347.400 ms`. A matching branch internal probe reported
  ordinary warm open/close at `138.025 ms`, open/startup at `112.727 ms`,
  close at `25.295 ms`, and active-runtime reconnect at `0.891 ms`. This keeps
  the current performance diagnosis focused on process-isolated embedded
  MariaDB/InnoDB lifecycle startup, not ownerless coordination setup and not
  active in-process reconnect.
  The current production branch probe after the transaction-page publish dedup
  slice reported ordinary active-runtime reconnect at `1.222 ms`, ownerless
  active-runtime reconnect at `0.971 ms`, ordinary prepared `SELECT 1` at
  `2466.47 ops/s`, ownerless prepared `SELECT 1` at `2154.50 ops/s`, ordinary
  autocommit inserts at `1190.87 ops/s`, and ownerless autocommit inserts at
  `767.28 ops/s`. The matching WordPress probe used the CI-pinned WordPress ref
  `6ddfc9d9b532c6e95c1266165149815895e2eb56` and reported stock PHP startup at
  `55.655 ms`, MyLite-extension PHP startup at `75.788 ms`, PHP process plus
  connect/close at `603.801 ms`, in-process connect/close at `393.118 ms`,
  active-runtime reconnect at `3.427 ms`, `SELECT 1` at `380.95 ops/s`, point
  selects at `228.71 ops/s`, transactional inserts at `385.62 ops/s`, prepared
  autocommit inserts at `315.41 ops/s`, and direct-string autocommit inserts at
  `722.09 ops/s`, so the current PHPUnit wall-time risk is repeated process
  lifecycle work rather than active reconnect or ordinary SQL throughput.
  The current CI production-build audit also requires the WordPress timing job
  to keep its Docker image, source fetch, PHP-extension build, dependency,
  database-prep, perf-probe, and split test-only PHPUnit phases separate, and
  rejects the old all-in-one harness phase for CI timing. The audit now checks
  the step bodies for embedded tests/probes, WordPress dependency/database/
  perf/PHPUnit phases, and clang tools, so those timing paths must retain their
  `Release` MyLite and `MinSizeRel` MariaDB embedded cache guards locally in
  the step that publishes timings. The normal non-isolated WordPress PHPUnit
  timing step keeps mysqli profiling disabled so its timing remains
  production-like; explicitly profiled diagnostic runs still copy selected
  `mylite_mysqli_profile_*` profile totals from captured PHPUnit output, using
  the last profile summary when bootstrap emits an earlier process-local
  profile. Process-isolated WordPress PHPUnit CI now enables the lightweight
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY` mode while keeping
  heavyweight child profiling off, so the shared timing summary reports
  parent-side child count, runtime, lock-release, and reconnect averages
  without injecting child-body instrumentation into the production timing
  shards. The embedded production probes now tee their logs into the
  `embedded-performance-reports` artifact, and the WordPress timing summary is
  uploaded as `wordpress-phpunit-timing-summary`, giving follow-up performance
  audits downloadable evidence without enabling heavyweight profilers in the
  default timing run. The WordPress runtime artifact now uses an explicit slim
  staging root that preserves the production CMake caches and manifest-hashed
  runtime files, prunes WordPress Git object history from the shard payload,
  validates the manifest from inside the staged tree, and reports staged-root
  byte size beside compressed tarball size so artifact transport overhead stays
  visible independently of PHPUnit execution. Shard jobs materialize the PHP
  runtime Docker image from the warmed BuildKit cache instead of downloading and
  `docker load`-ing a separate compressed image artifact, while still timing
  that image setup before the PHPUnit test-only step. The follow-up source snapshot
  keeps `src`, `tests/phpunit`, the writable REST fixture directory under
  `tests/qunit/fixtures`, root metadata, Composer autoload metadata, and Yoast
  PHPUnit polyfills while excluding non-PHPUnit test trees and root Composer
  development dependencies from shard fanout. The embedded ownerless
  SQL CI step now also prints the
  direct `sql-case` count, emits per-case start and status/seconds log markers,
  and appends a case-index timing table to the GitHub step summary while preserving
  the failing case's exit status. This keeps long ownerless correctness
  coverage tied to case-level performance evidence rather than a single opaque
  step duration. A fresh guarded
  sample on 2026-06-10 reported ordinary embedded warm open/close at
  `361.594 ms`, with `mysql_server_init()` at `125.794 ms` and
  `mysql_server_end()` at `228.758 ms`, while active-runtime reconnect stayed
  at `1.259 ms`. The matching WordPress probe reported process plus
  connect/close at `552.251 ms`, in-process connect/close at `382.206 ms`,
  active-runtime reconnect at `3.707 ms`, and `SELECT 1` at `410.02 ops/s`.
  This keeps the next optimization target on full embedded lifecycle cost and
  process-isolated child startup and any remaining parent reconnect policy, not
  the steady active-runtime SQL loop.
  Follow-up InnoDB startup attribution then split ordinary warm-open cost into
  steady recovery/bootstrap work and actual redo rebuild spikes. A reduced
  twenty-open production sample reported four actual redo rebuilds averaging
  `162.922 ms`; reason and physical-size counters showed size mismatch only,
  with both `log_sys.file_size` and physical `ib_logfile0` at `100663304`
  bytes versus the then-configured `100663296` bytes, matching redo format, and
  zero internal/physical divergence. That moves the startup optimization target
  to clean-shutdown redo tail handling or MariaDB checkpoint policy validation,
  not ownerless coordination and not an unsafe startup-only rebuild skip.
  Clean-shutdown redo-tail truncation then removed the repeated warm-open
  rebuild in the ordinary reduced probe. Follow-up recovery-start attribution
  split the remaining non-rebuild InnoDB startup cost: the ordinary
  five-iteration sample reported warm open/close at `120.698 ms`,
  `startup_innodb_srv_start_total_ms_avg=38.910`,
  `startup_innodb_recovery_start_ms_avg=18.930`,
  `startup_innodb_recovery_start_scan_initial_ms_avg=15.194`,
  `startup_innodb_recovery_start_scan_rescan_ms_avg=3.724`,
  `startup_innodb_recovery_trx_lists_ms_avg=2.931`, and
  `startup_innodb_system_tables_open_tmp_ms_avg=10.001`.
  Transaction-list restore is now present in compact summaries, but the
  measured fixed rollback-segment restore was smaller than clean redo scanning
  and temporary tablespace opening: 640 rollback-segment entries, five cached
  undo slots, zero active undo slots, zero prepared undo slots, and zero
  resurrected transactions.
  Temporary-tablespace startup attribution then split the
  `innodb_system_tables_open_tmp` bucket. The ordinary reduced production
  sample reported `startup_innodb_temp_tablespace_total_ms_avg=12.255`,
  dominated by repeated 12 MiB temp tablespace file create/open work
  (`startup_innodb_temp_tablespace_open_or_create_ms_avg=10.007`,
  `create_new_calls=5`, `reuse_existing_calls=0`) and followed by temporary
  rollback-segment creation
  (`startup_innodb_temp_tablespace_rseg_create_ms_avg=2.153`). The ownerless
  sample showed the same temp tablespace shape, while still occasionally
  paying actual redo rebuild time; redo rebuild trigger frequency remains a
  separate performance target from the temp file create/open cost.
  A follow-up temp sparse-size slice now uses sparse sizing for newly created
  InnoDB temporary tablespace files on non-Windows builds. A reduced production
  probe reported ordinary warm-open temp `open_or_create` at `0.084 ms` with
  `sparse_set_size_calls=2`, `create_new_calls=2`, and `reuse_existing_calls=0`;
  ownerless warm-open temp `open_or_create` was `0.065 ms` with the same counts.
  The remaining temp tablespace bucket is now temporary rollback-segment
  creation, not physical 12 MiB temp-file sizing.
  A temp rollback-segment attribution slice emitted per-open
  `trx_temp_rseg_create()` setup, header-create, in-memory reset, commit, call,
  and created-segment counters. At that point it kept native temporary rollback
  segment semantics unchanged while `trx_t::assign_temp_rseg()` still selected
  across all 128 `TRX_SYS_N_RSEGS` slots. The reduced production probe reported
  ownerless warm-open temp rseg creation at `2.117 ms`, with `2.039 ms` in
  header creation and 256 created temp rollback segments across two opens.
  A follow-up embedded temp rollback-segment pool slice now creates and assigns
  a 16-entry power-of-two prefix, preserving ownerless temporary-table SQL
  semantics while reducing per-process temp rollback-segment startup work. A
  reduced production probe reported ownerless warm-open temp rseg header
  creation at `0.309 ms` per open and 32 created temp rollback segments across
  two opens.
  A follow-up final-ownerless shutdown slice then made that redo-rebuild target
  payload-aware: when the closing ownerless runtime holds the startup lock,
  has no live ownerless peers, and the retained WAL has no page-version payload
  records, MyLite temporarily lets MariaDB run clean InnoDB shutdown so the
  clean redo-tail truncation path can normalize `ib_logfile0`; the saved
  redo-header repair path follows the same payload-aware gate. Retained
  page-image WAL payload records and live-peer closes continue using the
  existing crash-style ownerless shutdown policy. Focused lifecycle tests cover
  both ownerless-created repeated write closes and ordinary-created,
  metadata-only ownerless attach closes; the reduced five-open production probe
  reported `mylite_perf_ownerless_warm_open_close_startup_phase_innodb_log_rebuild_calls=0`,
  `mylite_perf_ownerless_warm_open_close_startup_phase_innodb_log_rebuild_total_ms_avg=0.000`,
  ownerless warm `startup_innodb_srv_start_total_ms_avg=41.034`, and
  ownerless warm open/close at `111.688 ms` versus ordinary at `113.698 ms`.
  Stats-enabled
  ownerless autocommit probes now also emit per-insert summary keys for
  page-version volume, native-support page ratio, page-publish and page-log
  append time, page-log payload/header byte volume, compact-sparse metadata
  versus nonzero data bytes for the total, index, and SYS buckets,
  varint-compact sparse record and byte counts, page-write refresh/publish
  time, commit-MTR publish time,
  InnoDB write-history time split by ownerless history-page lock, post-wait
  refresh, rollback-segment latch, history-list mutation, write-history MTR
  commit, ownerless rollback-segment-space dirty-page flush, page-type buckets
  for that flush, the page-type-bucket sum and ratio guard, unique versus
  duplicate history-flush page identity, persistent undo assignment/cache-reuse
  decisions, history cache eligibility, ownerless-blocked cache ratio, and
  ownerless release time, ownerless visibility time, row-insert time, and
  clustered B-tree insert time. The same reduced attribution probe now also
  enables deep InnoDB counters for the ordinary insert loops, emits raw
  ordinary transaction/autocommit deep counters, and summarizes ordinary
  autocommit baselines plus ownerless-minus-ordinary per-insert deltas for the
  commit, write-history, history-list, commit-in-memory, ownerless visibility,
  row-insert, row-insert subphases, row graph/index-entry subphases, and
  clustered low-level search, duplicate-check, row-level MTR commit, rare
  fallback, and optimistic or pessimistic B-tree phases. Optimistic B-tree
  attribution now further separates preflight, lock/undo, tuple insertion,
  reorganization, adaptive-hash update, lock update, and result counts. That
  keeps the next performance target tied to measured ownerless-specific deltas
  instead of shared MariaDB/InnoDB insert cost. The `e6efccff` production CI
  run after the clustered-low split reported ownerless-minus-ordinary deltas of
  `0.249 ms/insert` in clustered-low total, `0.116 ms/insert` in clustered
  optimistic B-tree insertion, `0.129 ms/insert` in row-level MTR commit, and
  `0.004 ms/insert` in `btr_pcur_open()` search, while duplicate-check, rare
  fallback, and pessimistic B-tree deltas stayed at zero. The final reduced
  local production sample after the optimistic B-tree split reported a
  `0.480 ms/insert` optimistic B-tree delta, with `0.476 ms/insert` in
  `btr_cur_ins_lock_and_undo()`, `0.002 ms/insert` in tuple insertion,
  `0.001 ms/insert` in preflight, zero reorg/adaptive-hash/lock-update deltas,
  one successful optimistic insert per row, and no fallback/error counts. A
  follow-up lock/undo split now separates setup, lock checking,
  predicate/record lock checks, undo reporting, system-field writes, skip
  counts, success counts, and error counts inside `btr_cur_ins_lock_and_undo()`.
  The first reduced local production sample with that split reported a
  `0.158 ms/insert` optimistic lock/undo delta, with `0.156 ms/insert` in
  `trx_undo_report_row_operation()`, `0.002 ms/insert` in record lock
  checking, zero setup/system-field-write deltas, one primary-leaf success per
  row, and no skip/error counts. The undo-report attribution split now
  separates persistent/temp undo assignment, cached-undo reuse, fresh undo
  creation, insert/update page reporting, undo-report MTR commit, success
  bookkeeping, page extension, and error classes. The reduced 100-row local
  production sample reported a `0.103 ms/insert`
  ownerless-minus-ordinary undo-report delta, with `0.113 ms/insert` in the
  undo-report MTR commit bucket, near-zero page-record encoding and success
  bookkeeping deltas, no assign/space/record-size/other errors, ownerless
  cached-undo hits at `0.810` per insert, and fresh undo creates at `0.190`
  per insert. The explicit transaction undo-elision slice reduces that path by
  skipping pre-commit rollback-segment `FIL_PAGE_UNDO_LOG` native-support
  page-version WAL for non-autocommit ownerless SQL transactions in the
  transaction's own rollback-segment space, after the active history-proof page
  check has already refused rollback-segment and undo pages needed by commit
  serialization. The explicit transaction visible-proof and history-proof
  slices now carry a conservative first-party proof from eligible
  `INSERT ... VALUES` writes to the later `COMMIT`, so the prepared
  explicit-transaction probe can publish visibility through the same fast
  commit gate instead of reporting `flush_unproven_statement`, and can publish
  the active rollback-segment and undo history pages instead of running the
  ownerless write-history page flush inside
  `trx_t::write_serialisation_history()`. A follow-up explicit transaction
  latest-checkpoint coalescing slice then keeps the same statement-local
  lifetime as visible-fast append batching and coalesces later non-durable
  latest-only checkpoint rewrites inside each proven insert statement after the
  first latest update has been preserved. The reduced stats-enabled sample for
  the history-proof slice reported explicit-transaction commit visibility at
  `fast=1.000`, `flush=0.000`, and `unproven=0.000` per transaction, history
  proof publication at `1.000` rollback-segment page and `1.000` undo page per
  transaction, zero write-history ownerless flush pages, and transaction-page
  publication at `3.000` image pages and `9.000` buffer pages per transaction.
  Stats-off throughput remains host-sensitive; the local 1000-row samples
  reported ownerless explicit transaction throughput at `2274.01` and
  `2374.21` ops/s, while ordinary explicit transaction baselines reported
  `3822.45` and `3841.90` ops/s. Savepoint-controlled transactions are
  deliberately disqualified from this proof and remain on the conservative
  unproven path, and focused coverage verifies coalescing stays disabled after
  that disqualification point. A later explicit-COMMIT append-batching slice
  marks only proof-backed COMMIT statements as append-batch eligible, wraps
  transaction-deferred COMMIT page publication in the native page-publish batch
  hooks, and lets external-snapshot-lineage records use append sessions, so
  COMMIT-time transaction-page and history-proof page-version records share a
  page-log append session without holding the append lock across user
  transaction work; the reduced production probe moved that phase from 10
  direct plus 2 session appends to 0 direct plus 12 session appends. Broader
  redo/checkpoint reconciliation remains separate follow-up work. That keeps
  the next remaining
  performance slices near native-support proof and InnoDB commit/row-insert
  work rather than PHP/PHPUnit startup or SQL row encoding. The generic InnoDB
  read-complete
  ownerless overlay now runs only for MyLite-classified plain `SELECT`/`WITH`
  page-version reads; non-SELECT DDL and DML rely on explicit page-write
  refresh and publication paths. The current reduced stats-enabled autocommit
  sample therefore reports zero non-SELECT ownerless page-read probes, while
  its remaining `0.080 ms/insert` page-log append and `0.115 ms/insert`
  commit-MTR publish costs point back to page-version/native-support write
  volume rather than accidental DDL/DML read overlay work. The page-log
  write-volume attribution probe reports raw payload, record-header, and total
  record bytes plus ownerless autocommit per-insert byte averages, keeping the
  production timing evidence tied to full-page WAL volume before a future WAL
  format or native redo/checkpoint proof attempts to reduce it. The
  ownerless page-log zero-range payload slice is the first such WAL-format
  reduction: it stores either a one-pass sparse nonzero-run payload for
  zero-heavy pages or the nonzero prefix for pages whose tail is all zero,
  flags the record, reconstructs the full page before checksum validation, and
  keeps all page-version records present for active readers, history-proof
  handoff, and no-live `.shm` rebuild. Tail-only encoding was neutral in the
  hot sample, while the sparse encoder reduced page-log payload bytes from
  `49479.680` to `7967.610` per simple ownerless autocommit insert without a
  measured append-time regression. This is intentionally not a native
  redo/checkpoint proof and does not skip the rollback-segment or undo-header
  history-proof records. A prepared DML reset fast path then removed a
  non-storage cost from the same production probe: 500 ownerless autocommit
  resets fell from `120.489 ms` total with `120.397 ms` in
  `mysql_stmt_reset()` to `0.064 ms` total with `0.000 ms` in
  `mysql_stmt_reset()`. The companion stats-off production sample reported
  ownerless autocommit at `1520.62 ops/s` and ownerless transactional inserts
  at `1567.77 ops/s`, but the stats-enabled run still showed the larger
  remaining costs in page-log append, commit-MTR page publication,
  write-history, and row-level MTR commit. A later text-execution slice removed
  the per-step native prepare/close path for eligible ownerless prepared
  no-result DML; the stats-enabled production probe then reported zero native
  prepare/close calls for ownerless autocommit prepared inserts, while the
  stats-off sample reported ownerless autocommit prepared inserts at
  `1349.90 ops/s` and ownerless transactional prepared inserts at
  `1506.99 ops/s` in that run. The
  write-history page-write handoff now uses a rollback-segment-space target-LSN
  wait instead of a global dirty-page wait,
  preserving native proof for the history page while avoiding unrelated
  user-table dirty pages that the later page-version fast path already covers;
  the first stats-off production sample after that change reported ownerless
  autocommit at about `408 ops/s` versus ordinary autocommit at about
  `1832 ops/s`, while the stats-enabled attribution sample still showed the
  rollback-segment-space flush as a remaining `~1.0 ms/insert` cost; a
  follow-up page-count attribution sample showed that wait flushing about
  `2.5` rollback-segment-space pages per insert, and the follow-up page-type
  profile splits that count into undo-log, index, FSP header, XDES, inode,
  allocated, system, transaction-system, and other buckets; the stats-enabled
  production attribution probe now fails if those buckets do not add up to the
  same ownerless flush total. The next ownerless history-flush identity profile
  classifies those successful native history flushes by unique page identity,
  duplicate page identity, duplicate page type, and bounded-table overflow, and
  the same stats-enabled probe fails if `unique + duplicate + overflow` does
  not add up to the ownerless flush total or if duplicate page-type buckets do
  not add up to duplicate pages. The follow-up ownerless undo-cache reuse
  profile instruments MariaDB's existing one-page cached-undo assignment and
  history-cache eligibility branches without changing behavior, so the
  production attribution run can distinguish fresh rollback-segment-space churn
  caused by the current ownerless guard from broader redo/checkpoint or
  page-publication costs. Its first reduced 100-row production attribution
  sample reported one ownerless cached-undo reuse skip, one fresh undo-log
  create, one size-eligible history record, one ownerless-blocked eligible
  history record, and zero cached history records per autocommit insert, with a
  blocked-ownerless ratio of `1.0000`. A bounded follow-up optimization now
  enables MariaDB's cached-undo path only when the same continuous single-owner
  proof used for external page-refresh skips succeeds. The current reduced
  100-row production attribution sample reported ownerless autocommit
  cached-undo attempts at `1.000` per insert, cache-reuse hits at `0.810` per
  insert, fresh creates at `0.190` per insert, history cached at `1.000` per
  insert, and an ownerless-blocked history-cache ratio of `0.0000`. The
  exact native history flush attribution now splits dirty-page needs checks,
  exact known-page flush try time, exact-write AIO wait time, the AIO wait's
  write-slot and doublewrite-buffer child waits, space-wide fallback time, and
  final redo-log write time. The earlier reduced production sample reported
  `2.000` exact history flush pages per insert and `0.000` fallback rounds per
  insert, with sampled cost dominated by exact page try/wait rather than
  fallback. A follow-up AIO child-wait profile reported
  `3.718 ms/insert` total exact AIO wait, `3.717 ms/insert` in
  `write_slots->wait()`, and effectively zero doublewrite-buffer wait, so the
  next runtime optimization needs broader native redo/checkpoint reconciliation
  or a cheaper native page-write proof rather than fallback-scan reduction or
  doublewrite-wait tuning. A follow-up queue-depth profile adds pending
  write-slot counts before and after that exact wait so production logs show
  whether the wait is target-page write latency or global queue drain; its first
  reduced sample reported `0.940` pending writes before the wait and `0.000`
  after the wait per insert while exact flush pages remained `2.000` per
  insert, so the bottleneck did not look like unrelated global queue drain. A
  bounded history WAL proof fast path now marks a transaction-local proof window
  for pure autocommit `INSERT ... VALUES` statements, disables native-support
  page WAL elision only for the expected rollback-segment and undo-header
  pages, and skips the native exact history flush only when both expected page
  images are published successfully through the ownerless page WAL. When another
  ownerless peer is live, `INSERT ... VALUES` with an explicit target-column
  list into AUTO_INCREMENT targets stays on the conservative refresh/flush
  bridge until page-version publication has a peer-safe merge proof for locally
  dirty user pages and auto-increment high-water state. The reduced
  production
  attribution sample after this change reported `0.000` ownerless history flush
  pages and `0.000` exact history flush pages per insert while keeping `3.570`
  native-support pages per insert and `1.570` native-support elided pages per
  insert; the companion stats-off production sample reported ownerless
  autocommit at `775.42 ops/s` versus ordinary autocommit at `2272.02 ops/s`,
  ratio `0.3413`. A follow-up transaction-page publish dedup slice skipped the
  page-id fallback only after a captured transaction page image had already been
  successfully published for that same page id. The reduced production
  attribution sample after that change reported `302` page-log appends per
  `100` ownerless autocommit inserts, `3.000` page-version records per insert,
  `3.570` native-support records per insert, `1.570` native-support elided
  records per insert, page-log append `0.080 ms/insert`, commit-MTR publish
  `0.115 ms/insert`, write-history `0.108 ms/insert`, row insert
  `0.153 ms/insert`, and clustered optimistic B-tree insert `0.070 ms/insert`.
  A follow-up native-support attribution slice split those remaining
  native-support pages by published versus elided page class. Its reduced
  stats-enabled production sample reported `2.000` published native-support
  pages per insert, exactly `1.000` undo page and `1.000` transaction-system
  page, with `0.000` published space-metadata pages; elided native-support
  pages included `1.000` undo, `0.380` space-metadata, and `0.190`
  transaction-system pages per insert. The next ownerless autocommit target is
  therefore reducing necessary history-related native-support publication
  volume or proving cheaper native commit/row-insert handoff, not a broad tuple
  dedup pass or broader space-metadata elision. A follow-up history-proof
  attribution slice proved the simple hot path's remaining published
  native-support pages are the exact history-proof pages: `1.000` published
  history-proof rollback-segment page and `1.000` published history-proof undo
  page per insert, matching the `1.000` published `FIL_PAGE_TYPE_SYS` page and
  `1.000` published `FIL_PAGE_UNDO_LOG` page per insert, with both pages also
  counted as blocked from blind native-support elision by the active
  history-proof gate. A future optimization must replace or compress that proof
  evidence rather than simply eliding these page images. A bounded follow-up
  kept proof-only native-support metadata durable in the page-version WAL while
  skipping live shared page-index publication for records that have no page
  payload. A later read-path performance slice re-indexed full native-support
  page images so single-owner readers can trust absent index entries for
  materializable pages without treating proof-only metadata as readable page
  images. Direct indexed hits also skip appended-tail validation in the
  single-owner one-statement epoch, including the local visible-fast writer,
  while the narrower single-owner absent-index case can avoid repeated WAL scans
  only when active ownerless native write state is absent. Peer/startup WAL
  readers keep the conservative tail-validation path. This removes repeated
  live WAL scans for index-proven hits and, when safe, absent pages in the
  single-owner path; it does not reduce the remaining page-version WAL append
  count. Its first reduced stats-enabled
  production sample reported `2.010` skipped native-support live-index publishes
  per autocommit insert and `0.002 ms/insert` in page-publish index time while
  page-log append remained `0.054 ms/insert`; the companion stats-off
  production sample reported ownerless single-row autocommit at `1628.66 ops/s`
  versus ordinary autocommit at `3353.20 ops/s`, ratio `0.4857`. A follow-up
  compact-sparse composition attribution sample reported `6701.610`
  compact-sparse payload bytes per ownerless autocommit insert, split into
  `1266.080` metadata bytes and `5435.530` nonzero data bytes; the SYS
  history-proof bucket was data-dominated at `51.200` metadata bytes and
  `4123.730` data bytes per insert, while the index bucket was roughly split at
  `1086.220` metadata bytes and `1216.060` data bytes per insert. That keeps
  the next write-throughput target on rollback-segment SYS proof representation
  and user/index page payload rather than compact sparse run metadata. A
  follow-up varint compact sparse page-log slice reduced the remaining compact
  metadata by encoding compact sparse run headers as varuint16 gaps and run
  sizes when that was smaller than the 16-bit compact form. Its reduced 100-row
  production attribution sample selected the varint format for all `3.020`
  compact-sparse records per ownerless autocommit insert, reduced page-log
  payload to `6078.160` bytes per insert, and cut compact-sparse metadata to
  `642.200` bytes per insert. Index payload fell to `1761.180` bytes per
  insert with `545.110` metadata bytes and `1216.070` data bytes, while SYS
  payload remained data-dominated at `4152.270` bytes per insert with only
  `28.560` metadata bytes. The larger remaining throughput target therefore
  stays on SYS proof data and remaining user/index nonzero payload. A
  follow-up direct-varint sparse encode slice keeps the same WAL bytes and
  record flags but builds selected varint compact sparse payloads directly
  during the page scan instead of materializing and reparsing discarded 16-bit
  compact payload bytes. The reduced stats-enabled production sample reported
  unchanged record counts and payload bytes, while autocommit append encode
  time moved from the prior `4.312 ms` sample to `3.574 ms` for `302` varint
  records and bulk append encode time moved from `2.757 ms` to `2.293 ms` for
  `152` varint records. Short-run total append and row-throughput samples
  remain noisy because payload-write timing varies; the next larger
  throughput target therefore still stays on SYS proof data, user/index
  payload, and native redo/checkpoint proof. A
  follow-up history-proof delta attribution slice appended accepted proof-page
  same-identity counters without changing page-version WAL format. Its reduced
  100-row production attribution sample reported `1.000` rollback-segment
  proof sample and `1.000` undo-header proof sample per ownerless autocommit
  insert, but only `0.010` same-identity diff samples per insert for each role
  in the four-slot diagnostic cache, with `0.950` evictions per insert. The
  observed same-identity diffs were tiny, `0.110` rseg changed bytes and
  `0.400` undo changed bytes per insert, but the page-log still carried
  `6077.410` payload bytes per insert, including `4152.330` SYS bytes and
  `1761.200` index bytes. The next proof-representation optimization therefore
  needs to address history-proof identity churn or broader redo/checkpoint
  proof semantics, not just a last-image delta for an immediately repeated
  proof page. A follow-up history-proof identity attribution slice added
  larger per-role fingerprint tables without storing full page images. The
  reduced stats-enabled production sample reported `1.000` unique
  rollback-segment proof identity and `1.000` unique undo-header proof identity
  per insert, with zero duplicate identities and zero table overflows for both
  roles. The bulk insert phase saw `24` unique identities and `1` duplicate
  for each role across `25` proof samples. The simple autocommit proof payload
  is therefore dominated by fresh identity churn, not same-page proof reuse
  hidden by the four-slot byte-diff cache. A follow-up fill-sparse page-log
  slice records repeated nonzero fill runs for `FIL_PAGE_TYPE_SYS` pages while
  preserving the full-page checksum and page-version proof contract. The
  reduced 100-row stats-enabled production sample reported the same `3.020`
  page-log append calls per ownerless autocommit insert, but total page-log
  payload fell to `2010.720` bytes per insert and SYS proof payload fell from
  the prior `4152.330` bytes to `72.010` bytes per insert. The new fill-sparse
  bucket accounted for `1.000` record per insert and split those SYS bytes into
  `46.030` metadata bytes, `23.980` raw-data bytes, and `2.000` fill bytes.
  Remaining payload is now dominated by `1779.030` index bytes and `159.340`
  undo-log bytes per insert, with page-log append at `0.084 ms/insert` and
  append encode at `5.386 ms` for the `302` records in the reduced sample. A
  100-row stats-off sample reported ownerless autocommit at `1518.08 ops/s`
  versus ordinary autocommit at `3834.32 ops/s`, while a 500-row stats-off
  sample reported ownerless autocommit at `1110.33 ops/s` versus ordinary at
  `3873.31 ops/s`. A follow-up index fill-sparse page-log slice extends that
  byte-exact format to `FIL_PAGE_INDEX` records while proving index records
  still require oldest-snapshot boundary retention. The reduced 100-row
  stats-enabled production probe still reported `1779.010` index bytes,
  `159.340` undo-log bytes, and `72.030` SYS bytes per insert, with
  fill-sparse selecting only the `1.000` SYS record per insert in the simple
  insert path. That proves the representative index image is not
  fill-run-dominated. The follow-up index fill-sparse prefilter inspects the
  already-built compact payload and requires an actual repeated fill run before
  attempting the full fill-sparse index encoding. The reduced 100-row
  stats-enabled production probe still selected only the `1.000` SYS
  fill-sparse record per insert and reported `1779.030` index bytes per
  insert, but append encode returned to `0.059 ms/insert` and page-log append
  to `0.088 ms/insert`, with ownerless autocommit at `1364.26 ops/s` versus
  ordinary at `2907.78 ops/s`. A follow-up SYS fill-sparse direct-encode
  slice then removed the discarded compact sparse materialization for
  `FIL_PAGE_TYPE_SYS` pages when the existing fill-sparse record already wins.
  The reduced 1000-row stats-enabled production probe preserved `3010`
  single-row autocommit page-log append calls, `1000` fill-sparse/SYS records,
  `1746847` payload bytes, and `72344` SYS payload bytes while append encode
  moved from the pre-slice `63.598 ms` sample to `58.064 ms`. Bulk append
  encode was `32.789 ms` with the same `1510` append calls, `250`
  fill-sparse/SYS records, and `1166401` payload bytes. A follow-up index page
  delta attribution slice added stats-only page-log identity reuse and
  changed-byte counters for
  `FIL_PAGE_INDEX` records. Its reduced 100-row stats-enabled production probe
  reported `0.990` duplicate index identities per ownerless autocommit insert,
  `0.020` unique index identities per insert, no size mismatches or table
  overflows, and only `39.860` changed bytes per insert while the index page
  WAL still wrote `1779.010` payload bytes per insert. The follow-up index
  delta page-log slice added a non-chained durable delta format, a stable
  publish-hook page image contract, system-tablespace exclusion, and standalone
  warm-up before delta selection. Its reduced 100-row stats-enabled production
  probe selected `90` index deltas and cut index page-log payload to `598.580`
  bytes per insert, compared with the `1779` byte baseline. The next
  follow-up bounded index-delta base-refresh slice capped cumulative
  same-base drift, and the later no-dirty publish-batch slice made the MTR
  no-dirty release-loop page-publication path use the same append-session batch
  contract as the made-dirty scan path. The reduced 200-row production
  attribution sample after that batching change reported `602` page-log append
  calls, `600` append-session records, only `2` direct append calls, append
  lock time `0.565 ms`, fstat time `0.561 ms`, and encode time `18.583 ms`.
  This removed a measurable append-batching inconsistency but still left
  page-log encoding as a first-party hot path. The follow-up index-delta fast
  path records each warmed base's standalone encoded payload size and
  fast-accepts bounded small deltas before building a new standalone payload.
  A 500-row production attribution sample after that slice reported `1506`
  page-log append calls, `470` index deltas, `402` fast-accepted index deltas,
  and page-log append encode time `30.980 ms`, down from the preceding
  `54.815 ms` 500-row sample. The first-base index-delta warm-up slice now
  lets one standalone index record seed later bounded non-chained deltas. Its
  reduced 500-row stats-enabled sample reported `0.962` index deltas per
  insert, `0.820` fast-accepted index deltas per insert, and `829.066` index
  payload bytes per insert, down from the preceding same-shape `927.956` index
  bytes per insert sample. A follow-up undo-log page-delta slice generalized
  the same durable non-chained/checkpoint-rewritten delta contract to repeated
  `FIL_PAGE_UNDO_LOG` records with an undo-specific record flag. Its reduced
  500-row production attribution sample selected `0.752` undo deltas and
  `0.728` fast-accepted undo deltas per insert, cut undo-log page-log payload
  from the preceding same-shape `539.910` bytes per insert to `210.006`, cut
  total page-log payload from `1442.294` to `1112.380` bytes per insert, and
  preserved `1.000` rollback-segment plus `1.000` undo history-proof
  publications per insert. A follow-up delta-base snapshot slice keeps the
  same durable non-chained delta record format but stores cached base pages as
  immutable shared vectors, so index and undo delta candidates no longer copy
  a 16 KiB base page out of the process-local base table before encoding. Its
  reduced 500-row production attribution sample preserved `1506` page-log
  appends, selected `0.962` index deltas and `0.752` undo deltas per insert,
  and reported `0.040` page-log encode ms/insert. A follow-up delta fast-limit
  slice raised the fast acceptance threshold from `1024` to `2048` bytes while
  keeping the half-standalone-size rule and exact fallback unchanged. The
  reduced 500-row production attribution sample kept payload bytes stable
  (`556202` versus `556208` before the slice), increased fast index deltas from
  `410` to `449`, dropped standalone-size probe calls from `91` to `51`,
  dropped size-probe time from `7.229 ms` to `1.157 ms`, and moved total
  page-log append time from `49.012 ms` to `44.932 ms`. A follow-up delta
  standalone-estimate slice refreshes the process-local fast-decision
  standalone-size estimate after exact fallback has computed the current size
  and the delta append succeeds. Its reduced 500-row production attribution
  sample kept page-log appends at `1506`, kept payload bytes flat (`556191`
  versus `556202`), increased fast index deltas from `449` to `468`, dropped
  exact index deltas from `32` to `13`, and dropped standalone-size probes from
  `51` to `30`. A follow-up SYS page-delta audit rejected automatic
  `FIL_PAGE_TYPE_SYS` deltas for now: a broad 500-row probe selected `0.752`
  SYS deltas per ownerless autocommit insert and reduced page-log payload from
  `1112.386` to `1081.608` bytes/insert, but the record-lock-grant crash hook
  recovered `SUM(value)=30` instead of `31`, and a narrower user-tablespace
  attempt still failed from the repository-root working directory. Primitive
  coverage now proves repeated `space_id=1` and `space_id=80` SYS records stay
  standalone and read back byte-identically. The remaining write-throughput
  targets are native
  commit/page-publication, non-fast page-log encoding, the remaining
  history-proof proof volume, and broader redo/checkpoint recovery work. A
  follow-up delta payload direct-copy slice keeps the same index/undo
  non-chained delta record bytes but copies all raw changed-byte runs into an
  already-sized payload buffer instead of repeatedly appending each run. This
  narrows a measured delta-encode CPU cost without changing WAL format,
  checkpoint rewrite, recovery, or page-version volume. A follow-up sparse
  payload direct-copy slice keeps the same standalone sparse WAL formats but
  copies compact-varint sparse and fill-sparse raw runs into resized vector
  tails instead of using range inserts. This narrows a measured standalone
  encode CPU cost without changing sparse record selection, payload bytes,
  replay, checkpoint rewrite, recovery, or page-version volume. A follow-up
  delta eligibility reuse slice classifies each appended page image once and
  reuses that delta flag for both process-local base snapshot lookup and
  post-append base-note update, skipping the note helper for page classes that
  cannot seed deltas. This narrows first-party append bookkeeping without
  changing WAL bytes, delta acceptance, replay, checkpoint rewrite, or
  page-version volume. A follow-up history-rseg page-type slice keeps the
  explicit `MYLITE_OWNERLESS_PAGE_LOG_APPEND_HISTORY_RSEG_DELTA` gate but lets
  hinted rollback-segment proof pages use the same history-rseg delta for
  MariaDB's `FIL_PAGE_TYPE_TRX_SYS` page class as well as `FIL_PAGE_TYPE_SYS`.
  Primitive coverage proves hinted SYS and TRX_SYS reconstruction/latest lookup
  and checkpoint rewrite to standalone records, while ordinary unhinted system
  pages stay standalone and the required rollback-segment plus undo-header
  proof publication remains unchanged. A follow-up delta note slot-reuse slice
  carries the matched process-local delta-base slot index from snapshot lookup
  into the successful post-append note path and revalidates that slot for both
  accepted deltas and exact-fallback standalone base refreshes before falling
  back to the old fingerprint/probe loop. Primitive coverage exercises the
  standalone-refresh branch through the internal
  `delta_base_standalone_slot_reuse_records` diagnostic. This keeps the same
  volatile base-cache rules and durable WAL semantics while trimming duplicate
  cache lookup work. A follow-up delta-base buffer reuse slice preserves the
  same cache admission, durable records, payload bytes, replay, and checkpoint
  behavior, but standalone base refreshes now reuse an existing unshared cached
  page buffer instead of allocating a new page vector. Primitive coverage
  asserts the `delta_base_page_buffer_reuse_records` diagnostic and production
  probes expose it for attribution. A four KiB delta fast-path follow-up raises
  the bounded fast acceptance cap from `2048` to `4096` bytes while retaining
  the half-standalone-size rule, exact fallback above the cap, non-chained
  delta records, checkpoint rewrite, and replay behavior. Primitive coverage
  proves the medium fast-accepted index-delta and larger exact-fallback
  boundary both read back byte-identically. The reduced 500-row four-row-bulk
  production probe moved fast-limit rejections from `41` to `6`,
  standalone-size probe calls from `174` to `144`, standalone-size probe time
  from `3.074 ms` to `1.838 ms`, and page-log append total from `8.411 ms`
  to `7.183 ms`. A later MTR wrapper fast-path audit rejected caching
  `ownerless_page_write_uses_transaction_release()` per publish pass or
  reusing the tracked-page lookup for release after ownerless stress found
  reader monotonicity failures and a DDL stress InnoDB assertion in prototype
  builds; that path needs stronger proof before being retried. The
  visible-fast page-log append-batch slice
  narrows another measured overhead source by keeping the append session open
  across the adjacent mini-transactions of a single-row pure
  `INSERT ... VALUES` visible-fast statement, then releasing it before
  page-log sync and page-visible LSN publication. The first wider
  implementation attempt regressed the production bulk-insert phase, so
  multi-row append-session deferral remains future work. A follow-up
  ownerless page-write leave
  membership fast path now checks the MTR-owned ownerless page-write vector
  before resolving transaction and deferred-release policy for page latch
  slots. The reduced 500-row production attribution sample kept page-version
  counts stable at `3.008` per insert and moved
  `page_write_leave_total_ms` from `5.554` to `4.911`, with the sampled
  no-dirty commit-log loop moving from `76.541 ms` to `65.279 ms`; this is a
  bounded hook overhead reduction, not a replacement for the larger native
  commit/page-publication and redo/checkpoint work. The made-dirty MTR commit
  path now collects modified page pointers during MariaDB's existing
  flush-list pass and publishes that collected list after commit-log and
  ownerless-redo latch release, avoiding the later full MTR memo scan used
  only to rediscover modified pages. A reduced production attribution sample
  after the change kept `3.000` MTR-published pages and `3.020` page-log
  appends per ownerless autocommit insert while moving
  `page_write_publish_scan_calls_per_insert` and
  `page_write_publish_scan_ms_per_insert` from the prior `1.275` / `0.049`
  sample to `0.000` / `0.000`; this preserves page-version volume and leaves
  the larger native commit, history-proof publication, and page-log append
  costs as remaining targets. A follow-up
  ownerless page-publish buffer-reuse slice keeps the same synchronous
  full-page publish hook, checksum initialization, page-type attribution, and
  history-proof publication semantics, but reuses one aligned transient page
  buffer per publishing thread and physical page size instead of
  allocating/freeing a scratch page for every published record. The embedded
  performance probe emits publish-buffer reuse hit/miss counters in detailed
  page-write stats and compact ownerless autocommit summaries. This reduces
  hot-path allocation churn but does not change page-version volume or the
  remaining redo/checkpoint proof gap. A follow-up page-log append attribution
  slice keeps the WAL format unchanged and splits stats-enabled append time
  into delta-base snapshot lookup, delta encoding, standalone encoding,
  payload stats bookkeeping, page-type stats bookkeeping, and delta-base
  note-update counters so future work can distinguish stats-only profiling cost
  from runtime page-log append work. The embedded performance probe now keeps
  append timing, append byte, and payload-format counters enabled while making
  detailed page-type and index-identity attribution opt-in through
  `MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1`; the default
  stats-attribution mode prints
  `mylite_perf_ownerless_page_log_detail_stats=0` so CI timings are not
  dominated by the classifier used only for byte-composition investigations.
  A follow-up redo hook attribution slice promotes existing database perf
  counters into compact production summaries for ownerless redo enter, observe,
  reserve, written, and leave work. The reduced 200-row stats-enabled sample
  reported autocommit redo enter/reserve/written/leave at `3.000` calls per
  insert with redo-leave `0.022 ms/insert`, while the explicit transaction
  probe reported `401.000` enter/reserve/written/leave calls per transaction
  and redo-leave `3.159 ms/transaction`. That separates redo hook overhead from
  page-write publish (`0.094 ms/insert`), commit-log publish
  (`0.095 ms/insert`), page-log append (`0.058 ms/insert`), and checkpoint
  update (`0.024 ms/insert`) in the same autocommit sample. The explicit
  transaction latest-checkpoint coalescing follow-up reported `40` coalesced
  latest-only checkpoint rewrites for `40` prepared inserts, while leaving
  autocommit coalescing at `2.000` per insert and four-row bulk coalescing at
  `8.000` per statement. A reduced stats-off 400-row local sample reported
  ordinary explicit transactions at `4168.89 ops/s`, ownerless explicit
  transactions at `2880.92 ops/s`, ratio `0.6911`.
  A follow-up first-party database perf stats-off slice made database and
  embedded-open elapsed helpers return immediately on their disabled zero
  sentinel, avoiding the redundant second disabled-flag load in normal
  production, WordPress PHPUnit, and stats-off embedded timing paths while
  keeping enabled diagnostic counters unchanged.
  The follow-up native MTR page-write diagnostic gate applies the same
  principle inside `mtr_t::ownerless_page_write_enter()`,
  `mtr_t::ownerless_page_write_leave()`, and
  `mtr_t::ownerless_page_write_publish()`: the functions now return through
  their existing inactive-hook, zero-commit-LSN, startup, or recovery checks
  before loading the page-write perf flag or constructing elapsed-time scopes.
  Enabled ownerless counters still cover active page-write locking and publish
  work, but ordinary and inactive MTR paths no longer pay disabled diagnostic
  overhead. This slice deliberately leaves page-version volume, latch release
  ordering, and redo/checkpoint reconciliation unchanged.
  A follow-up record-wait attribution slice adds first-party database perf
  counters for `ownerless_innodb_lock_wait_until_record_hook()` calls, elapsed
  time, and OK/timeout/unavailable/error result classes, then emits compact
  per-insert, per-row, per-statement, and bulk first/remaining summaries. This
  times the insert-intention availability probe reached from MariaDB
  `lock_rec_insert_check_and_lock()`, not the explicit record
  acquire/release hook. The final 16K-row local production attribution sample
  reported `16384` ownerless bulk record wait-until calls, `24.478 ms` total,
  all OK, and zero timeout/unavailable/error results; in the remaining bulk
  statement this explains most of the `29.046 ms` ownerless
  `row_ins_btr_lock_undo_rec_lock` bucket, but larger remaining gaps still sit
  in row insert (`190.280 ms` ownerless-minus-ordinary), undo report
  (`76.220 ms`), undo-report MTR commit (`70.608 ms`), and page-write
  commit-log work (`58.450 ms`) per statement. A follow-up
  `ownerless-single-owner-record-wait-skip` slice now bypasses that shared
  record-lock registry availability probe only after the process registry
  proves a continuous single-owner epoch through `active_count == 1` and
  generation equality with the current owner slot, and after the record-lock
  registry proof finds no waiting entries or foreign active record owner.
  MariaDB's local record-lock check still runs first, live-peer and prior-peer
  generations keep the shared registry path, a same-process synthetic external
  ownerless record lock still publishes a visible shared wait, and dedicated
  counters report skip calls, allowed skips, unmapped blocks, active-count
  blocks, and stale-generation blocks. This is a bounded follow-up reduction,
  not the primary remaining parity fix. The final 16K-row production probe
  after the skip reported the same `16384` logical ownerless bulk wait-until
  calls with `16384` allowed single-owner skips and reduced wait-until time to
  `1.098 ms` per remaining statement; a later corrective reduced probe after
  the record-registry guard still reported `16384` allowed skips and
  `1.723 ms` remaining record wait-until time. Remaining row insert
  (`285.786 ms`), undo report (`104.685 ms`), undo-report MTR commit
  (`77.828 ms`), and page-write commit-log work (`58.187 ms`) stayed larger
  targets.
  Reduced 50-row stats-enabled probes after
  that split showed both standalone encoding and delta-base note update can be
  visible inside the previous aggregate append total, with small-sample ranking
  too noisy to pick a WAL-semantic change without a larger run. The
  post-boundary production sample before this proof fast path reported stats-off
  ownerless warm open/close at `359.230 ms` versus ordinary `375.478 ms`,
  active-runtime reconnect overhead at `0.211 ms`, ownerless direct/prepared
  read ratios of `0.9008`/`0.8629`, ownerless transactional insert ratio of
  `0.7110`, and ownerless autocommit at `601.94 ops/s` versus ordinary
  `2001.12 ops/s`, ratio `0.3008`. The companion 100-row stats-enabled
  attribution run reported ownerless autocommit at `407.20 ops/s` versus
  ordinary `2130.37 ops/s`, ratio `0.1911`, `4.570` page-version records per
  insert, `3.570` native-support records per insert, `0.433 ms/insert` in
  page-log append, and `0.561 ms/insert` in rollback-segment-space dirty-page
  flush. Focused SQL coverage includes `single-owner-history-wal-proof` for the
  exact history-page WAL proof and a stale-generation selector proving the
  single-owner proof blocks after another ownerless process has joined and
  left. Broader cached undo reuse after peer history remains disabled until a
  shared rollback
  segment cache protocol proves stale history-list links,
  rollback-segment header state, and page ownership safe across live peers.
  CI keeps
  the default embedded performance
  probe as the stats-off throughput signal and runs a separate reduced
  stats-enabled ownerless attribution probe under the same `php-embedded-prod`
  production build, including the ordinary-versus-ownerless deep-counter
  deltas. It now also runs a reduced append-only attribution probe with
  `MYLITE_PERF_OWNERLESS_APPEND_STATS=1` and page-publish stats disabled, so
  CI-visible production logs include history-proof pair calls and page-log
  append timing from the fast path where
  `mtr_t::ownerless_history_proof_publish_pair()` remains active. A later
  embedded text-query drain fast path removes a redundant
  `mysql_next_result()` no-more-results probe from ordinary single-result
  `mylite_exec()` and `mylite_exec_result()` calls while keeping the
  `mysql_more_results()`-guarded stored-procedure/multi-result drain path;
  focused embedded exec, PHP mysqli, ownerless read/commit selectors, and a
  reduced ownerless stress pass cover the change. This is a broad query
  plumbing cleanup for WordPress-shaped text SQL, not a replacement for the
  remaining ownerless redo/checkpoint and page-publication performance work.
  Ownerless statement startup now fast-checks the already-observed stable
  dictionary generation before taking the full dictionary ready-wait path. Any
  active DDL owner, odd generation, changed generation, unreadable mapping, or
  uninitialized observation still falls through to the existing wait and
  dictionary-cache refresh logic. This reduces repeated statement-boundary
  overhead for hot reads and small writes without weakening dictionary
  generation invalidation.
  Ownerless direct and prepared execution now coalesces visible-fast commit
  and page-log append-batch policy classification into one private result. For
  `INSERT ... VALUES`, the row-list shape is parsed once and target
  foreign-key state is resolved once before applying the existing visible-fast
  and one-through-four-row append-batch predicates; when another ownerless peer
  is live, explicit-column-list AUTO_INCREMENT targets use the conservative
  current-read refresh bridge instead of carrying stale dirty pages across
  statement boundaries; explicit `COMMIT` keeps the transaction-scoped
  visible-fast proof and never enables append batching.
  The explicit transaction proof is also extended to constrained single-table
  `UPDATE ... SET ... WHERE ...` statements when the target table is not
  involved in referential constraints and the statement has no joins, subquery,
  savepoint, locking-read, DDL, or dictionary-refresh disqualifier. Focused SQL
  coverage proves prepared simple updates can use fast COMMIT visibility and
  the rollback-segment/undo history WAL proof, while a subquery update remains
  on the conservative unproven path.
  The same proof is extended to constrained single-table
  `DELETE FROM ... WHERE ...` statements when the target table has no
  referential constraints, no trigger for the delete operation, and no joins,
  subquery, alias, modifier, `RETURNING`, or partition shape.
  Focused SQL coverage proves prepared simple deletes can use fast COMMIT
  visibility and the rollback-segment/undo history WAL proof, while subquery
  deletes and trigger-bearing delete targets remain conservative.
  The update/delete proof now also accepts constrained single-table
  `ORDER BY`/`LIMIT` variants with a `WHERE` clause, while preserving the
  existing subquery, join, alias, trigger, referential-constraint, partition,
  and no-`WHERE` exclusions. Focused SQL coverage proves prepared
  ordered/limited update and delete statements can share the fast COMMIT
  visibility and rollback-segment/undo history WAL proof.
  The proof is also extended to constrained single-table
  `REPLACE ... VALUES` statements when the target table has no referential
  constraints, no triggers, no auto-increment column, and no `REPLACE ...
  SELECT`, modifier, explicit partition target, `RETURNING`, or joined shape.
  Focused SQL coverage proves prepared simple replacements can use fast COMMIT
  visibility and the rollback-segment/undo history WAL proof, while
  `REPLACE ... SELECT`, trigger-bearing replacement targets, and
  auto-increment replacement targets remain conservative.
  The same explicit-transaction proof is now covered for mixed proven DML: one
  transaction can combine prepared `INSERT ... VALUES`, constrained
  single-table `UPDATE`, constrained single-table `DELETE`, and constrained
  single-table `REPLACE ... VALUES` statements, while a later mixed transaction
  containing one unproven subquery update still falls back to the conservative
  unproven-statement path.
  The page-write publish summary slice then promoted existing detailed native
  page-write counters into CI-facing per-insert summary rows for publish
  calls, dirty-page scan work, deferred pages, lookup/allocation/copy/checksum/
  hook/free time, and commit-log flush-list/release/redo-leave/publish/
  release-memo/no-dirty-loop subphases. This is diagnostics-only and keeps the
  next write-path optimization selectable from production CI logs.
  The transaction-release classification fast path then cached
  mini-transaction-local transaction-release and page-deferral decisions in the
  ownerless write-enter, page-publish, no-dirty commit-log, and unlogged
  release loops. It preserves lock, refresh, publication, boundary, and
  history-proof behavior while avoiding repeated helper work on
  statement-visible autocommit writes.
  The follow-up page-write enter classification reuse slice returned that
  already-computed write-enter predicate to modified-page entry sites and let
  commit-publication paths pass a precomputed `transaction_publish` result into
  dirty-page tracking. It is another hot-path cleanup only; page-version WAL,
  native history-proof publication, checkpoint ordering, and ownerless lock
  release semantics stay unchanged.
  The single-pass delta page-type slice then removed repeated InnoDB page-type
  probes from ownerless page-log append classification. A page image is still
  eligible for the same index, undo, or explicit history rollback-segment delta
  record formats as before; the classifier now loads `FIL_PAGE_TYPE` once and
  reuses it for those decisions. System-space index pages, unhinted SYS/TRX_SYS
  records, record flags, payload bytes, checkpoint rewrite, replay, and native
  history-proof requirements are unchanged.
  The standalone size-probe single-pass slice then removed the second
  size-only page scan for index/SYS fill-sparse candidates in exact fallback.
  The helper still applies the same compact, varint compact, fill-sparse, and
  trailing-zero selection rules before accepting or rejecting a retained
  fast-miss delta; new primitive coverage forces a fill-sparse standalone
  rejection to guard against overestimating the standalone size. The reduced
  500-row production sample preserved append counts, payload bytes, and delta
  counts while reducing the one-row standalone-size probe time in the first
  post-slice sample; bulk timing remained noisy and is not treated as a
  material throughput improvement.
  The delta exact negative-cache slice then records exact fallback standalone
  rejections in the volatile delta-base slot. A later append with the same base
  slot skips the exact standalone-size probe only when the new fast decision is
  also a standalone-size rejection; fast-limit misses still reach exact
  fallback because existing primitive coverage proves they can be accepted by
  the exact comparison. Primitive coverage proves the second repeated
  rejection stores standalone bytes, skips the probe, and replays the current
  page byte-identically. A reduced 120-row production sample reported one
  skipped exact standalone probe in both the single-row autocommit and
  four-row bulk shapes, leaving the larger ownerless write target on native
  page publication, history/native durability proof, and redo/checkpoint
  reconciliation.
  The four KiB delta fast-path follow-up raises the bounded fast acceptance
  threshold from `2048` to `4096` bytes while keeping the cached-standalone
  half-size rule and exact fallback for larger deltas unchanged. Primitive
  coverage proves a medium index-page delta above `2048` bytes uses the fast
  path without a standalone-size probe and replays byte-identically, while a
  larger delta above `4096` bytes still records a fast-limit miss, reaches
  exact fallback, and replays byte-identically. WAL flags, payload
  reconstruction, checkpoint rewrite, replay, and SQL semantics are unchanged.
  On the same reduced 500-row four-row-bulk production shape, fast-limit
  rejections dropped from `41` to `6`, standalone-size probe calls from `174`
  to `144`, standalone-size probe time from `3.074 ms` to `1.838 ms`,
  page-log encode time from `6.142 ms` to `4.977 ms`, page-log append total
  from `8.411 ms` to `7.183 ms`, and ownerless bulk throughput stayed in the
  same range at `4880.07` to `4904.94` rows/s.
  The delta exact reuse-probe cache then records positive exact fallback reuse
  observations in the same volatile delta-base slot. A later same-base
  fast-limit miss with an outstanding positive observation can accept the
  already-built delta payload without repeating the standalone-size probe;
  standalone rejection, build failure, base refresh, invalidation, and
  max-delta bounds stay conservative. Primitive coverage proves the first
  exact reuse still probes, the next same-base append skips the probe and
  replays byte-identically from the delta record, and the following append
  probes again after consuming the single positive observation. Final reduced
  simple and 100-row-bulk stats-enabled production probes still used
  standalone-size probes for their representative exact-reuse records, so the
  current measured CI-shaped workloads should not claim a throughput win from
  this cache. WAL format, checkpoint rewrite, replay, and concurrency ordering
  remain unchanged; the tradeoff is bounded WAL-size risk if a skipped probe
  would have selected a smaller standalone record.
  The page-log metadata flag coalescing slice then added a single helper that
  reads one page-log record header and returns the metadata bits needed by
  native checkpoint proof collection. The proof collector now tests
  snapshot-boundary, external-lineage, and marked native-support state from
  that one header read, while unmarked records still fall through to the legacy
  payload classifier before they can become user-page proof records. Page-log
  bytes, marker preservation, checkpoint proof rules, replay, and recovery
  behavior are unchanged.
  A no-dirty leave-exhaustion fast path then made the commit-log memo release
  loop stop calling `ownerless_page_write_leave()` after the MTR-owned
  page-write vector has become empty. Pages that are still recorded in the
  vector keep the same ownerless release path, and native memo release,
  page-latch release, page publication, redo, checkpoint, and recovery
  semantics are unchanged.
  The same no-dirty release loop then added a caller-side page-write
  membership precheck while that vector is still non-empty. Non-member X/SX
  page memo slots no longer enter `ownerless_page_write_leave()` only to
  return before release; member pages still use the existing helper and release
  order.
  The release-memo membership precheck then applies the same non-empty-vector
  and page-membership guard to generic `mtr_t::release()` and
  `release_unlogged()` ownerless page-write leave calls. The reduced 500-row
  four-row-bulk production probe preserved page-version, native-support,
  page-log append, and commit-visibility counts while moving bulk leave calls
  from `1485` to `1458`, leave total from `2.101 ms` to `1.989 ms`,
  and release time from `1.854 ms` to `1.685 ms`; release-memo,
  no-dirty-loop, page-log append, and throughput timings remained noisy across
  short final probes. This is a small release-path cleanup and still leaves
  history/native proof volume and redo/checkpoint reconciliation as the larger
  write-throughput targets.
  Ownerless page-write publication now snapshots the page-write perf-enabled
  flag once per publish call and uses that snapshot for publish-call,
  publish-total, subphase, and scratch-buffer reuse counters. This removes
  repeated diagnostics-only flag loads from stats-disabled production paths
  while preserving page publication, history-proof marking, page-log append,
  checkpoint, and recovery semantics.
  Ownerless page-publish loops then made page-publish batch hooks lazy: the
  collected-page list publisher, full MTR memo publisher, and no-dirty
  commit-log publish loop now call
  `mylite_ownerless_innodb_begin_page_publish_batch()` only before the first
  immediate `ownerless_page_write_publish()` in that pass, and call the
  matching end hook only when the begin hook ran. Transaction-deferred pages
  still record and capture dirty page images in the same latch order, while
  deferred-only passes avoid the ownerless batch hook pair. Page-version WAL,
  native-support/history-proof publication, append-session batching for actual
  page appends, checkpoint ordering, and recovery semantics are unchanged.
  Its local production stats-enabled sample preserved explicit transaction
  publication at `2` page versions and one page-log append session begin/end
  pair per transaction, autocommit publication at `3.008` page versions per
  insert and `2.004` native-support pages per insert, and the four-row bulk
  shape at one page-log append session begin/end pair per statement. The
  matching stats-off sample reported ownerless explicit transactions at
  `0.6435x` ordinary, ownerless autocommit at `0.4558x`, and ownerless
  four-row bulk rows at `0.4223x`.
  Dirty transaction-page capture then reused the transaction-publish
  classification already computed by the collected-page publisher, full MTR
  memo publisher, and no-dirty commit-log publish loop. The generic capture
  entry point still checks hook state and recomputes the predicate, while
  preclassified callers skip that duplicate predicate work before the capture
  helper revalidates the transaction pointer, source page, transaction-owned
  page set, and page LSN. Dirty-page ownership, captured image contents,
  page-version publication, WAL format, checkpoint ordering, and recovery
  behavior are unchanged.
  Its stats-enabled production sample preserved explicit transaction
  publication at `2` page versions per transaction, `6` transaction image
  publishes, `4` transaction buffer publishes, and one page-log append session
  begin/end pair around the actual transaction page appends. Reduced stats-off
  throughput samples were noisy, so this slice records unchanged publication
  counters and removed duplicate predicate work rather than claiming a stable
  throughput win.
  The transaction-image in-place preparation slice then removed the temporary
  page-sized vector allocation and copy inside
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()`. Captured page
  images are private to the committing transaction and cleared during
  transaction cleanup, so the publisher can run the same InnoDB checksum
  preparation directly on that buffer before calling the page-version hook.
  Transaction-image publication counts, page-id fallback, WAL format,
  checkpoint ordering, and recovery semantics stay unchanged.
  The page-write stats-off fast-path slice then made disabled page-write
  elapsed scopes return on the existing zero start-time sentinel before
  rechecking the stats-enabled flag. Focused SQL coverage proves disabled
  page-write counters stay zero through the native-support WAL proof workload,
  and the stats-enabled attribution probe still emits page-write summary rows.
  The first stats-off sample after the cleanup reported ownerless autocommit at
  `1046.79 ops/s` versus ordinary autocommit at `3673.54 ops/s`, ratio
  `0.2850`, so this is a bounded diagnostics hot-path cleanup rather than
  evidence that the larger write-throughput gap is solved.
  A follow-up page-publish stats-off fast path then hoisted the disabled
  page-publish attribution decision once per native publish call and skips
  diagnostics-only page-type, identity, history-proof, and result counter
  helper calls while stats are disabled. The focused native-support SQL
  selector now performs a stats-disabled ownerless write and verifies
  representative page-publish counters remain zero, while enabled attribution
  counters remain covered by the same selector and production probe.
  A matching commit-visibility stats-off fast path now returns immediately
  from disabled elapsed helpers on the zero timing sentinel and reuses one
  stats-enabled snapshot for visible-fast and conservative-flush reason
  counters inside the ownerless commit block. Focused visible-fast SQL coverage
  proves a stats-disabled ownerless write leaves representative
  commit-visibility counters at zero, while enabled commit-visibility
  attribution remains covered by the same selector and production probe. Commit
  visibility publication, log flush, dirty-page flush, page-visible LSN
  publication, lock release, and recovery semantics are unchanged.
  The checkpoint legacy-write elision slice then removed the legacy
  latest/visible payload write from advancing checkpoint updates after
  checksum-protected LSN records are initialized, exposing a
  `checkpoint_update_legacy_write_elided` counter while preserving durable sync
  ordering and fail-closed torn-record recovery.
  The page-log write summary slice promoted existing append lock, fstat,
  header/body setup, checksum, payload write, and record-header write counters
  into compact CI-facing summaries. It does not change page-log write order;
  payloads still reach disk before record headers so crash recovery cannot see
  a valid header for a missing payload.
  A follow-up native-support record-marker slice adds a page-log metadata bit
  for ownerless native-support page-version records after the publish hook has
  already classified the page image. Native checkpoint proof replay and
  oldest-snapshot boundary checks can skip payload decoding for marked records,
  while unmarked records fall back to the legacy page-type decode path. The
  marker survives retained-record checkpoint rewrites and does not change
  payload bytes, checksums, page-version ordering, or the remaining
  rollback-segment/undo history-proof requirement.
  A follow-up proof-only native-support WAL slice uses that metadata class for
  active history-proof rollback-segment and undo records. It keeps page
  identity, page LSN, commit LSN, native-support metadata, and retention
  ordering, but writes no page payload and forbids page-image reads from those
  records. Replay and checkpoint retained-record callbacks skip them for the
  same reason the live page-index path skips proof-only native-support records.
  The append-batch fault-guard slice then narrowed unsafe-hook checks to
  actual configured ownerless fault names. Hook builds still enable fault
  infrastructure globally, but visible-fast correctness selectors and
  production-shaped performance probes now keep page-log append batching and
  deferred latest-checkpoint coalescing unless `MYLITE_OWNERLESS_TEST_FAULT`
  names an active fault. Named fault runs remain conservative so page-publish
  and checkpoint crash windows are individually observable.
  The eight-row visible-fast append-batch slice then raises the parser-proven
  `INSERT ... VALUES` cap from four to eight rows after an eight-row production
  probe showed the statement already used visible-fast commit publication but
  fell back to per-mini-transaction append sessions and disabled deferred
  latest-checkpoint coalescing. A bounded sixteen-row follow-up applies the
  same proof to the next measured bulk shape after a production probe reported
  visible-fast commit at `1.000` per statement but `171` append-session
  begin/end calls for `10` statements and zero coalesces under the eight-row
  cap; the post-slice production probe kept the same `185` page-log appends
  but reduced append-session begin/end calls to `10`, enabled `320` deferred
  latest-checkpoint coalesces, and moved ownerless 16-row bulk throughput from
  `7223.47` to `10655.43` rows/s on the same local shape. A bounded
  thirty-two-row follow-up applies the same proof to the next measured row
  list after the 32-row baseline reported visible-fast commit at `1.000` per
  statement but `331` append-session begin/end calls for `10` statements and
  zero coalesces under the sixteen-row cap; the post-slice production probe
  kept the same `345` page-log appends but reduced append-session begin/end
  calls to `10`, enabled `640` deferred latest-checkpoint coalesces, and moved
  ownerless 32-row bulk throughput from `8345.99` to `9307.03` rows/s on the
  same local shape. A bounded sixty-four-row follow-up applies the same proof
  after the 64-row baseline reported visible-fast commit at `1.000` per
  statement but `651` append-session begin/end calls for `10` statements and
  zero coalesces under the thirty-two-row cap; the post-slice production probe
  kept the same `669` page-log appends but reduced append-session begin/end
  calls to `10`, enabled `1280` deferred latest-checkpoint coalesces, and moved
  ownerless 64-row bulk throughput from `9856.67` to `11755.40` rows/s on the
  same local shape. The streamed row-count follow-up then replaces the fixed
  policy-token row counter with a full SQL-text scan so larger row lists cannot
  be append-batched through token-window undercounting; 128-row and 256-row
  production probes at the previous head had already shown `10` append-session
  begin/end calls for `10` statements, visible-fast commit at `1.000` per
  statement, and deferred latest-checkpoint coalescing at `256.000` and
  `512.000` per statement, so the explicit cap moves to 256 with focused
  coverage for the 256-row positive boundary and the 257-row conservative
  boundary. A statement-deferred page-publish follow-up then reuses the
  existing transaction-deferred page publication proof for those same bounded
  append-batched statements while the process registry proves a single-owner
  epoch, so repeated user data/index page images are captured and replaced by
  `(space_id,page_no)` until commit instead of appended at every ownerless
  mini-transaction. Peer-present ownerless statements keep immediate page
  publication while retaining the append-session batching guard. A 256-row
  production probe
  moved page-log append calls from `2609` to `68`, page-write publish total
  from `60.789 ms` to `2.938 ms`, commit-log publish attribution from
  `61.565 ms` to `6.767 ms`, and ownerless 256-row bulk throughput from
  `12178.41` to `20008.38` rows/s while keeping visible-fast commit at
  `1.000` per statement and conservative flush at `0.000`; before the 512-row
  follow-up, the 257-row guard shape still reported `2581` append-session
  begin/end calls, `2619` page-log appends, and `2582` snapshot-boundary page
  publications for `10` statements. A bounded 512-row follow-up raises only the
  explicit pure
  row-list cap after the same full SQL-text proof: a reduced 512-row baseline
  reported `5131` append-session begin/end calls, `5193` page-log appends,
  `5144` snapshot-boundary page publications, zero deferred latest-checkpoint
  coalesces, and `11181.55` ownerless rows/s for `10` statements; the
  post-slice probe reported `10` append-session begin/end calls, `90` page-log
  appends, zero snapshot-boundary page publications, `1024.000` deferred
  latest-checkpoint coalesces per statement, and `25810.38` ownerless rows/s
  while keeping visible-fast commit at `1.000` per statement and conservative
  flush at `0.000`. A 1024-row follow-up keeps the same parser-proven
  policy for the next bounded row-list edge: the focused selector now proves
  the 1024-row positive boundary and the 1025-row conservative boundary.
  The reduced 10240-row production probe with 1024 rows per statement reported
  `10` append-session begin/end calls for `10` statements, `0`
  snapshot-boundary publications, `2.000` page versions and native-support
  proof pages per statement, `1.000` visible-fast commits per statement, and
  `2049.000` deferred latest-checkpoint coalesces per statement.
  A 2048-row follow-up keeps the same parser-proven policy for the next bounded
  row-list edge: the focused selector now proves the 2048-row positive boundary
  and the 2049-row conservative boundary. The reduced 20480-row production
  probe with 2048 rows per statement reported `10` append-session begin/end
  calls for `10` statements, `0` snapshot-boundary publications, `2.000` page
  versions and native-support proof pages per statement, `1.000` visible-fast
  commits per statement, and `115.200` deferred latest-checkpoint coalesces per
  statement. That same sample kept ownerless/ordinary bulk rows ratio at
  `0.3477`, with the first statement at `1.3253` and later statements at
  `0.3195`, leaving native row-insert/undo-report attribution as the next
  larger write-performance target. Larger row-list admission remains planned
  separately so this slice does not infer unbounded append-lock deferral from
  the bounded proof.
  A 4096-row follow-up keeps the same parser-proven policy for the next bounded
  row-list edge: the focused selector now proves the 4096-row positive boundary
  and the 4097-row conservative boundary. Before the cap increase, a reduced
  8192-row probe with 4096 rows per statement stayed outside append batching,
  with `8195` append-session begin/end calls for two statements, `4136.000`
  page versions per statement, `4138.500` page-log appends per statement,
  `21.000` native-support published pages per statement, zero deferred
  latest-checkpoint coalesces, and `9587.08` ownerless rows/s. After the cap
  increase, the same reduced shape reported `2` append-session begin/end calls
  for two statements, `2.000` page versions per statement, `17.500` page-log
  appends per statement, `2.000` native-support published pages per statement,
  `1.000` visible-fast commits per statement, `128.000` deferred
  latest-checkpoint coalesces per statement, ownerless `mysql_query()` at
  `66.720 ms` per statement, and `58904.02` ownerless rows/s. Larger row-list
  admission remains planned separately so this slice does not infer unbounded
  append-lock deferral from the bounded proof.
  An 8192-row follow-up keeps the same parser-proven policy for the next
  bounded row-list edge: the focused selector now proves the 8192-row positive
  boundary and the 8193-row conservative boundary. Before the cap increase, a
  reduced 16384-row probe with 8192 rows per statement stayed outside append
  batching, with `16387` append-session begin/end calls for two statements,
  `16458` snapshot-boundary page publications, `8268.000` page versions per
  statement, `8270.500` page-log appends per statement, `39.000`
  native-support published pages per statement, zero deferred latest-checkpoint
  coalesces, and `11128.17` ownerless rows/s. After the cap increase, the same
  reduced shape reported `2` append-session begin/end calls for two
  statements, zero snapshot-boundary page publications, `2.000` page versions
  per statement, `27.500` page-log appends per statement, `2.000`
  native-support published pages per statement, `1.000` visible-fast commits
  per statement, `256.500` deferred latest-checkpoint coalesces per statement,
  ownerless `mysql_query()` at `130.506 ms` per statement, and `61240.24`
  ownerless rows/s. Larger row-list admission remains planned separately so
  this slice does not infer unbounded append-lock deferral from the bounded
  proof.
  A 16384-row follow-up applies the same parser-proven policy to the next
  bounded row-list edge: the focused selector now proves the 16384-row
  positive boundary and the 16385-row conservative boundary. The reduced
  32768-row production probe with 16384 rows per statement reported `2`
  append-session begin/end calls for two statements, zero snapshot-boundary
  publications, `2.000` page versions per statement, `45.000` page-log appends
  per statement, `2.000` native-support published pages per statement,
  `1.000` visible-fast commits per statement, `513.000` deferred
  latest-checkpoint coalesces per statement, ownerless `mysql_query()` at
  `220.533 ms` per statement, and `72415.68` ownerless rows/s. The later
  non-empty-table statement still shows remaining native row-level undo/MTR
  cost, including `301.249 ms` row-insert time and `74.959 ms` undo-report MTR
  commit time per statement, so row lists above that edge and native undo work
  remained planned at that boundary.
  A 32768-row follow-up applies the same parser-proven policy to the next
  bounded row-list edge: the focused selector now proves the 32768-row
  positive boundary and the 32769-row conservative boundary. The reduced
  65536-row production probe with 32768 rows per statement reported `2`
  append-session begin/end calls for two statements, zero snapshot-boundary
  publications, `2.000` page versions per statement, `1598.500` page-log
  appends per statement, `2.000` native-support published pages per statement,
  `1.000` visible-fast commits per statement, `685.000` deferred
  latest-checkpoint coalesces per statement, ownerless `mysql_query()` at
  `422.337 ms` per statement, `60797.81` ownerless rows/s, and an
  ownerless/ordinary bulk ratio of `0.9550`. The later non-empty-table
  statement still shows remaining native row-level undo/MTR cost, including
  `115.835 ms` page-write commit-log time, `28.976 ms` redo-leave time, and
  `153.153 ms` undo-report MTR commit time per statement, so row lists above
  32768 and native undo work remain planned separately.
  A 65536-row follow-up applies the same parser-proven policy to the next
  bounded row-list edge: the focused selector now proves the 65536-row
  positive boundary and the 65537-row conservative boundary. The reduced
  131072-row production probe with 65536 rows per statement reported ownerless
  two-statement bulk throughput at `87019.79` rows/s against ordinary
  `157447.10` rows/s (`0.5527` ratio). The first 65536-row statement stayed
  close to ordinary at `148537.52` rows/s against `151011.68` rows/s
  (`0.9836` ratio), while the remaining non-empty-table statement reported
  `62337.71` rows/s against ordinary `171418.80` rows/s (`0.3637` ratio).
  The same sample exposed the larger remaining single-row autocommit gap at
  `435.56` ownerless ops/s versus `3420.87` ordinary ops/s (`0.1273` ratio),
  so row lists above 65536, native undo work, and single-row autocommit
  performance remain planned separately.
  A follow-up transaction page-membership cache reduces repeated exact
  modified/dirty page lookups from linear scans to lazy per-transaction
  open-addressed sets once page vectors reach 16 entries. The vectors remain
  authoritative and are still used for collection, cleanup, and predicate
  scans; cache entries are rebuilt after transaction gate erasure. The reduced
  2048-row production probe preserved the same append-batch shape, kept
  remaining default-checked bulk starts at `0.000` and remaining undo-report
  calls at `2048.000` per statement, and moved later-statement
  ownerless/ordinary rows ratio from `0.3195` to `0.3347`. Remaining
  row-insert time moved from `55.134 ms` to `51.364 ms` per statement and
  remaining undo-report MTR commit time from `16.498 ms` to `14.724 ms` per
  statement, so the next larger performance target is still native row-level
  undo work rather than page-log append sessions.
  The history-proof publication harness then tightens the controlled fast-path
  and unsafe fallback selectors without changing production code: rollback-
  segment proof publication must match published native-support
  `FIL_PAGE_TYPE_SYS`, undo proof publication must match published
  `FIL_PAGE_UNDO_LOG`, proof-only native-support records must be counted as
  skipped from live page-index publication, and forced native-support publish
  failure must take positive native history flush pages with zero accepted
  proof samples. A reduced stats-enabled production probe over 100 ownerless
  autocommit inserts reported `100` rollback-segment proof pages, `100` undo
  proof pages, matching `100` published native-support `FIL_PAGE_TYPE_SYS` and
  `100` published `FIL_PAGE_UNDO_LOG` pages, zero native history flush pages on
  the fast path, and a page-index native-support skip summary of `2.030` per
  insert.
  The native-support proof-only WAL slice then preserves those two proof
  records per eligible commit while removing their page-image payloads from the
  WAL. The reduced 100-row stats-enabled production sample reported `2.000`
  proof-only native-support records per ownerless autocommit insert, zero
  MTR scratch allocation/copy/checksum time for the proof path, and `503.380`
  page-log payload bytes per insert. The matching 500-row stats-off sample
  reported ownerless autocommit at `1950.43 ops/s` versus ordinary autocommit
  at `3560.43 ops/s`. This narrows proof representation cost only; it does not
  claim broader native redo, checkpoint, DDL/file-lifecycle, or SQL-level
  table-wait completion.
  A later default-checked bulk-insert slice reuses MariaDB's existing
  `TRX_UNDO_EMPTY` buffered empty-table path for ownerless autocommit direct
  `INSERT ... VALUES` row lists only when the ownerless statement policy proves
  the narrower single-owner deferred-page-publish path and the table has exactly
  one clustered InnoDB index and no foreign-key relationships, even if SQL
  `unique_checks` and `foreign_key_checks` remain at default values. The focused
  selector proves the ownerless-only bulk-start counter, duplicate-key statement
  rollback, CTAS and `INSERT ... SELECT` exclusion, peer-present cross-process
  exclusion, and preserved visible-fast publication; secondary indexes,
  foreign-key tables, explicit transactions, duplicate-handling DML, and broader
  SQL bulk coverage remain out of scope.
  The follow-up bulk exec-result attribution slice does not change the
  ownerless write path; it enables the existing `mylite_exec()` counters around
  ordinary and ownerless bulk autocommit loops in stats-enabled performance
  probes and emits compact per-statement `mysql_query()`, store-result,
  status-update, and exec-call summaries. That keeps CI evidence from hiding a
  multi-millisecond statement interval outside the already-reported page-log,
  page-write, commit-visibility, SQL-handler, and InnoDB-handler buckets.
  The text-execution attribution follow-up splits ownerless `mylite_exec()`
  timing across policy checks, pressure checks, statement locks, external-page
  refresh, dictionary DDL state, native `mysql_query()`, post-state updates,
  page-write release, checkpoint/visibility release, and statement-end reclaim.
  A production 100-row-per-statement append-attribution sample showed
  statement-end reclaim consuming `63.820 ms` across ten ownerless bulk
  statements while native `mysql_query()` consumed `22.396 ms`, proving the
  missing interval was foreground reclaim rather than MariaDB row execution.
  The same slice lets below-budget single-owner writes defer pending native
  marker reclaim to timer/no-live/close cleanup; a stats-off production sample
  moved ownerless row-list bulk throughput from about `11049.40 rows/s`
  (`0.1149` ratio) before the change to `44485.00 rows/s` (`0.4063` ratio)
  after the change, with a final verification run at `39648.94 rows/s`
  (`0.4131` ratio) and append attribution showing statement-end reclaim down
  to `0.022 ms` across ten ownerless bulk statements.
  The redo written/leave fusion slice then narrows a measured ownerless
  mini-transaction hot path by letting top-level production redo ranges complete
  the reserved range and publish the latest LSN through one shared redo-state
  progress-latch pass. The old written and leave callbacks remain the fallback,
  and unsafe named-fault hook builds keep the separate callbacks so redo crash
  windows remain individually observable. The pre-slice 5000-row,
  100-row-per-statement attribution baseline reported ownerless bulk at
  `18594.73 rows/s`, ordinary bulk at `108178.15 rows/s`, ownerless
  `mysql_query()` at `4.895 ms` per statement, and commit-log redo-leave at
  `0.988 ms` per statement. The post-slice sample reported ownerless bulk at
  `19629.59 rows/s`, an ownerless/ordinary ratio of `0.2052`, ownerless
  `mysql_query()` at `4.614 ms` per statement, page-write commit-log at
  `1.382 ms` per statement, and commit-log redo-leave at `0.737 ms` per
  statement.
  The active-reservation count follow-up then removes the O(64) reservation
  table scan from redo-state snapshot and leave policy by maintaining a
  progress-latch-protected counter at offset `88` in redo-state segment version
  `9`. It keeps reservation ordering, completed-range draining, checkpoint
  files, page-version WAL, and SQL semantics unchanged. A reduced production
  stats-off 30000-row, 100-row-per-statement probe reported ownerless bulk at
  `21802.61 rows/s`, ordinary bulk at `95543.15 rows/s`, and an
  ownerless/ordinary ratio of `0.2282`; this remains a bounded bookkeeping
  reduction, not a full ownerless write-throughput fix.
  The completed-range count follow-up uses offset `92` in redo-state segment
  version `10` to skip the completed-range table scan when no out-of-order
  ranges exist. Recording, merging, and draining completed ranges maintain the
  count under the same redo progress latch; redo ordering, completed-range
  coalescing, checkpoint files, page-version WAL, and SQL semantics are
  unchanged.
  The redo-leave subphase attribution follow-up then splits the existing
  `page_write_commit_log_redo_leave` bucket into native `log_write_up_to()`
  time, MyLite redo-state hook time, written-range hook calls, fallback hook
  calls, and zero-LSN leaves without changing redo write or redo-state hook
  order. A reduced 100-row bulk attribution sample preserved `2.000` page
  versions, `4.500` page-log appends, `2.000` native-support published pages,
  fast commit visibility, and `180.500` deferred latest-checkpoint coalesces
  per statement while reporting `0.671 ms/statement` aggregate redo leave,
  split into `0.362 ms/statement` native log-write time and
  `0.281 ms/statement` MyLite redo-state hook time.
  The inline MTR page-tracking follow-up keeps the first MTR-scoped ownerless
  page-write identity in `mtr_t` and uses the existing vector only for overflow
  pages. This preserves page-write release order, transaction-deferred
  ownership, native-support/history-proof publication, and native latch release
  while removing the overflow-vector allocation from common single-page MTR
  tracking. A reduced 100-row bulk attribution sample preserved `2.000` page
  versions, `4.500` page-log appends, `2.000` native-support published pages,
  fast commit visibility, and `180.500` deferred latest-checkpoint coalesces
  per statement while reporting `91.200` inline first-page records and only
  `1.000` overflow vector allocation per statement; timing remained noisy, so
  the slice is an allocation-path reduction rather than a broad throughput
  claim.
  The known-MTR-page leave follow-up then routes release loops that already
  proved a memo slot is MTR-tracked through a helper that skips the repeated
  membership checks while preserving the same forget/release, deferred-release,
  and native latch order. Reduced 100-row bulk samples preserved `2.000` page
  versions, `4.500` page-log appends, `2.000` native-support published pages,
  and fast commit visibility; post-change no-dirty page-leave timing was noisy
  at `0.204` to `0.220 ms/statement`, so this remains a bookkeeping cleanup
  rather than a throughput-completion claim.
  The proof-pair append follow-up then specializes the already-paired
  native-support history-proof hook for the two existing proof-only WAL records.
  When the ordinary page-log append session is active, the hook writes the same
  rollback-segment and undo proof headers directly as zero-payload
  native-support proof-only records, keeps append/session counters as record
  counts, coalesces the adjacent proof-only headers into one physical
  record-header write, and falls back to the previous generic two-append path
  when session setup is unavailable. Primitive coverage proves latest/read
  rejection, replay skipping, checkpoint retained-callback skipping, adjacent
  proof-only offsets, logical counter parity, and one `record_header_write_calls`
  sample for the pair API; focused history-proof SQL still proves successful
  pair publication and zero ownerless history-flush fallback in normal builds.
  This narrows proof append overhead only; it does not change WAL format,
  visible LSN rules, redo/checkpoint ordering, or DDL/file-lifecycle recovery.
  Larger row lists, broad DML/DDL, and unbounded append-lock hold times remain
  out of scope.
  The proof-only readable-WAL scan follow-up keeps uncheckpointed-record
  detection file-size based, but changes open/close retained-payload decisions
  to scan complete page-log records and ignore proof-only metadata records.
  Proof-only-only WAL therefore remains durable and retained without enabling
  ordinary native page-log read handling or retained-payload shutdown policy.
  Mixed logs still report readable page-version WAL as soon as any complete
  non-proof record is present, and incomplete tail bytes after proof-only
  records do not count as readable page payload evidence.
  The visible-fast redo batch-completion follow-up then defers top-level
  mini-transaction redo completion inside the existing statement deferred
  page-publication boundary. It writes native redo once per bounded batch,
  completes each reserved range through the existing fused written/leave hook
  before page-visible publication, statement teardown, or hook reset, and keeps
  unsafe hook builds, non-visible-fast statements, nested redo, and DDL/file
  lifecycle paths on the immediate path. The reduced stats-enabled 100-row bulk
  attribution sample reported ownerless `mysql_query()` at `3.406 ms` per
  statement, page-write commit-log at `0.725 ms` per statement, commit-log
  redo-leave at `0.260 ms` per statement, zero immediate page-write
  `log_write_up_to()` calls, `162.000` written/leave events per statement, and
  zero fallback hook calls. The matching stats-off 5000-row,
  100-row-per-statement production sample reported ownerless bulk at
  `27297.54 rows/s`, ordinary bulk at `96781.94 rows/s`, and a `0.2821`
  ownerless/ordinary ratio. This is a visible-fast hot-path reduction only; it
  does not close broader redo/checkpoint reconciliation or DDL/file-lifecycle
  recovery.
  The redo-state batch-completion follow-up then moves those deferred ranges
  through a first-party batch written/leave API, acquiring the shared redo
  progress latch once per bounded batch instead of once per range. It preserves
  the existing single-range fused hook as the fallback and keeps page-write
  counters as logical mini-transaction event evidence. A reduced stats-enabled
  100-row bulk sample reported database redo written/leave callbacks falling
  from `810` to `29` across five statements, while logical page-write
  written/leave events stayed at `162.000` per statement, commit-log redo-leave
  moved from `0.260 ms/statement` to `0.094 ms/statement`, and page-write redo
  hook time moved from `0.243 ms/statement` to `0.079 ms/statement`. The
  matching stats-off sample reported ownerless bulk at `33432.91 rows/s`,
  ordinary bulk at `107188.95 rows/s`, and a `0.3119` ownerless/ordinary ratio.
  This remains a bounded visible-fast redo-state hot-path reduction; it does
  not close broader redo/checkpoint reconciliation, SQL execution residuals, or
  DDL/file-lifecycle recovery.
  A larger deferred-redo range follow-up raises the fixed InnoDB hook buffer
  from `32` to `48` ranges, still below the shared redo state's `64`
  active-reservation slots. Embedded hook coverage fills the entire `48`-range
  batch and verifies one batch callback with full completed-count reporting,
  while the focused visible-fast SQL selector continues to prove database-level
  redo leave callbacks are fewer than logical page-write events. The accepted
  16384-row production rerun moved database-level redo written/leave callbacks
  from `1029`/`1028` to `686`/`685`, page-write redo hook time from `6.060` to
  `5.703 ms/statement`, and ownerless/ordinary bulk ratio from `0.6971` to
  `0.7717`. This reduces callback and progress-latch churn for large
  visible-fast row lists without changing native redo bytes, page-visible
  ordering, WAL format, or group-commit policy.
  A later bounded range-cap follow-up raises the fixed deferred-redo buffer from
  `48` to `61` ranges, deliberately below the `64` shared redo-state slots after
  accounting for the active-owner entry and two peer headroom slots. Primitive
  coverage proves a full configured batch still leaves room for a newly arriving
  peer to enter and reserve one redo range, while embedded hook coverage proves
  the full `61`-range batch is delivered as one batch callback. The accepted
  100-row stats-enabled page-publish attribution probe reported database redo
  written/leave callbacks at `39`/`38` across ten bulk statements while
  preserving `181.800` logical page-write written-hook events per statement,
  `2.000` page-version records per statement, and `32.300` page-log append calls
  per statement; stats-off local samples remained noisy, so this is recorded as
  a bounded callback/progress-latch reduction rather than a broad throughput
  claim.
  The follow-up bulk engine attribution slice is diagnostic-only: stats-enabled
  production probes now collect ordinary bulk SQL-handler, InnoDB-handler, and
  deep InnoDB counters and emit compact ownerless-minus-ordinary bulk deltas for
  native commit, history write, row insert, clustered insert, undo report, and
  default-checked bulk-start buckets. This closes the measurement gap between
  ownerless bulk `mysql_query()` time and ordinary baseline engine work without
  changing SQL behavior, page-version publication, redo/checkpoint ordering, WAL
  format, or recovery.
  The follow-up bulk undo phase attribution slice keeps the same diagnostic
  boundary but expands the compact first/remaining rows to existing B-tree
  lock/undo and undo-report subcounters. A reduced 2048-row production probe
  reported later ownerless statements at `20.857 ms` per statement in the
  B-tree lock/undo undo-report subphase, `20.606 ms` in undo-report total,
  `13.523 ms` in undo-report MTR commit, and `4.671 ms` in persistent undo
  assignment, with `2048.000` undo-report successes and `0.000`
  default-checked bulk starts per statement. A one-statement guard reported
  zero remaining-statement averages, so the remaining phase is not inferred
  from aggregate counters when no non-empty-table statement exists.
  The native-support publish fast-skip follow-up is a stats-off production
  dispatch cleanup: when diagnostics are disabled and the existing
  native-support predicate proves the publish helper would only elide a page,
  MTR publish dispatch skips the helper after checking the page image LSN
  against the mini-transaction commit LSN. Diagnostic runs still use the full
  helper so native-support elision and history-proof counters keep their
  existing meaning. Page-write locking/release, native undo records, redo
  completion, transaction-deferred user page publication, and active
  rollback-segment/undo history-proof pages are unchanged.
  The native-support lock-hold follow-up then reduces repeated page-write
  acquire/release churn without relying on the single-owner observation as a
  correctness proof. Autocommit visible-fast statements may keep an actually
  acquired shared page-write lock to transaction cleanup for native-support
  pages that the existing native-support elision predicate accepts. The held
  page list is separate from the transaction modified/dirty/page-image lists, so
  it does not participate in commit-time page-version publication or deferred
  page-write visibility proof. Transaction page-write release clears that
  membership when it releases the shared locks, including deadlock retry and
  rollback/forget paths.
  The held native-support publish-skip follow-up then reuses that
  transaction-local held-page proof to avoid a redundant publish-helper dispatch
  for pages that have no active rollback-segment or undo history-proof role and
  are already in the held native-support page-write list. Page-publish
  diagnostics still force the full helper so native-support elision and
  history-proof counters keep their existing meaning, while the production
  performance probe now supports `MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1` to
  expose the new `native_support_transaction_publish_skipped` counter without
  enabling page-publish stats. A reduced 1000-row page-write-only bulk sample
  reported `20` held native-support page-write locks, `1822` already-held hits,
  and `901` held-publish skips; a page-publish stats phase-split sample kept
  `2.000` published native-support proof pages, `100.000` elided
  native-support pages, and `0.000` held-publish skips per remaining 100-row
  statement. Page-write lock lifetime, native undo, history-proof WAL,
  redo/checkpoint ordering, page-version WAL format, and SQL behavior are
  unchanged.
  The transaction page last-hit cache follow-up keeps the authoritative
  modified, dirty, and held native-support page vectors plus their lazy exact
  sets unchanged, but remembers the last exact positive membership result for
  each class. Repeated hot-page checks in non-empty bulk row/undo
  mini-transactions can return from that process-local positive cache, while
  misses, vector/set rebuilds, clear paths, lock ownership, page-version
  publication, redo/checkpoint ordering, and rollback semantics stay on the
  existing paths.
  The held native-support hit fast-path follow-up then moves that
  already-held membership check earlier in ownerless page-write prepare/enter
  classification. Repeated undo and rollback-segment mini-transactions that
  touch a native-support page already retained by the visible-fast transaction
  can return before broader transaction-release classification. The held-page
  vector, native-support elision predicate, rollback-segment/undo history-proof
  publication, native undo records, page-version WAL, redo/checkpoint ordering,
  and rollback semantics are unchanged; this is a per-MTR bookkeeping reduction,
  not a non-empty-table undo elision.
  The reduced 100-row stats-enabled attribution sample kept `2.000` page
  versions and `2.000` published native-support pages per remaining statement,
  reduced enter-path held native-support hit accounting from `202.000` to
  `102.000` per remaining statement as prepare classification skipped the
  later enter hook, and moved ownerless bulk `mysql_query()` from
  `3.431 ms/statement` to `2.912`. The same local sample moved the
  ownerless/ordinary bulk rows ratio from `0.2966` to `0.4640`; the stats-off
  5000-row production probe reported ownerless bulk at `34064.01 rows/s` and a
  `0.3391` ratio. Remaining undo-report MTR commit time stays open as the
  larger native undo target.
  The bulk page-write phase split follow-up is diagnostic-only. The embedded
  performance probe now snapshots the first ownerless row-list statement for
  existing page-publish, database hook, page-write, page-log append, and
  commit-visibility counters and subtracts it from aggregate totals to emit
  `first_` and `remaining_` phase rows. A reduced two-statement 100-row
  production probe reported the later non-empty-table statement at
  `0.438 ms` page-write commit-log time, including `0.105 ms` redo leave,
  `0.178 ms` commit-log publish, `0.244 ms` no-dirty loop,
  `0.157 ms` no-dirty page-publish, `0.010 ms` page-leave, and `0.011 ms`
  page-unlock time per statement. A one-statement guard emitted `0.000`
  remaining phase averages, so the remaining rows are not inferred from
  aggregate counters when there is no later statement.
  The ownerless page-write deep attribution follow-up adds a lighter
  `MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1` probe mode that enables only
  InnoDB deep counters and first/remaining bulk snapshots. It keeps
  page-publish, page-write, page-log, database-hook, and exec-result
  diagnostics disabled, then emits ownerless page-write enter/acquire,
  native-support hit, transaction-owned/dirty skip, and page-image capture
  rows. This targets the remaining non-empty-table `trx_undo_report_mtr_commit`
  attribution gap without claiming a throughput fix or undo elision.
  The ownerless page-image last-hit cache follow-up is the first bounded
  optimization from that attribution. Transaction-deferred page-image capture
  now remembers the last positive vector index and reuses it only after
  validating that the index is still in range and still names the same packed
  page. Invalid, stale, and different-page cache states use the existing
  `std::find_if()` scan, and inserts or fallback hits refresh the cache.
  Deep probe rows now report capture-image cache hits and misses. This reduces
  repeated lookup work for hot bulk pages while preserving page-write
  ownership, native undo/redo, page-version publication, page-log bytes,
  checkpoint ordering, rollback, recovery, and peer visibility behavior.
  The ownerless MTR commit deep-attribution follow-up then splits the remaining
  broad `mtr_t::commit()` buckets into redo write, freed-page processing,
  commit-log, final resource cleanup, dirty flush-list insertion, latch
  release, ownerless redo leave, ownerless publish, memo release, and no-dirty
  loop rows in the production deep-stats probe. This is diagnostic
  instrumentation only: it keeps native undo, redo, page-version WAL,
  checkpoint ordering, lock lifetime, SQL behavior, and normal stats-off
  execution unchanged while identifying the next safe write-path target.
  Focused gating coverage proves active live writers, including idle explicit
  transactions between statements, and active snapshot pins keep WAL retained
  before close.
  The random transaction rollback handoff slice closes the highest-risk
  remaining transaction-boundary hole for shared-table ownerless stress:
  explicit transactions retain page-write ownership until transaction end,
  active page-write pages are skipped by background publication, COMMIT
  publishes only validated transaction page images plus rollback-segment/undo
  history proof, and full ROLLBACK refreshes tracked transaction pages from
  native storage instead of publishing rollback images or advancing visible
  LSN. SQL-layer rollback with local writes clears page-version read state and
  installs a native-read fence, while reader-only no-live close retains peer
  WAL appended after that runtime opened so later startup/rebuild can
  materialize the native boundary. Focused live snapshot reader-close coverage,
  including the synthesized native-boundary variant, now proves that a stale
  reader close retains the peer WAL after its pin releases, then a fresh
  ownerless opener reads the committed rows and checkpoints the retained WAL.
  Focused production coverage includes the
  explicit transaction history-proof selectors, uncommitted-peer-hidden,
  registered three-round random rollback handoff CTest, adjacent savepoint,
  deadlock, and commit-race commands, 100 traced and 50 untraced direct
  three-round random stress loops, reduced transaction stress, six-round random
  stress, and one default random stress pass. This is transaction rollback and
  handoff evidence only; broader redo/checkpoint reconciliation, arbitrary DDL
  file-lifecycle recovery, active-reader pressure crash/oracle breadth, and
  external MariaDB/RQG stress remain open.
  The killed-before-savepoint-rollback follow-up adds a focused
  killed-session proof for an explicit transaction that writes one
  file-per-table row before a savepoint, writes another row after it, and is
  killed before rollback or commit. No-live ownerless recovery, forced `.shm`
  rebuild, and ordinary native reopen preserve the pre-transaction rows while
  both native file-operation markers remain clear. This narrows the savepoint
  crash matrix, but does not claim a kill inside InnoDB savepoint rollback or
  concurrent-writer savepoint schedules.

  The current completion order is:

  1. Broaden native redo/checkpoint reconciliation and live-peer
     DDL/file-lifecycle recovery beyond the now-covered plain
     non-temporary `CREATE TABLE` prefinish boundary, especially crash recovery
     for table-copy created, rebuilt, renamed, truncated, and dropped
     file-per-table tablespaces while peers remain live.
  2. Close remaining transaction crash windows, especially kills inside
     rollback/savepoint rollback and concurrent-writer savepoint schedules
     that combine native undo, ownerless page-write ownership, and
     file-operation marker cleanup.
  3. Extend active-reader pressure evidence from retained-WAL policy to crash
     and external-oracle breadth for the high-risk DML/DDL classes already
     covered by bounded pressure policy tests.
  4. Continue deterministic external MariaDB seed/replay expansion and graduate
     to longer randomized MariaDB/RQG-style runs once the bounded recovery
     gates above stop producing new correctness issues.
  5. Keep production performance parity visible while those correctness slices
     land, with startup, native engine, ownerless write-path, and PHPUnit
     timing tracked in CI as separate build and test phases.

  SQL-level local table-wait fault injection is no longer listed as a primary
  completion gate for supported ownerless SQL: ownerless `LOCK TABLES` and
  SQL locked-table mode remain explicitly unsupported, representative blocked
  DDL shapes are negative-proofed as stopping before the local
  `table-lock-wait` callback, and the reachable supported SQL path is covered
  through the external native table-wait registry plus killed-waiter cleanup.
  Positive SQL reachability for the local table-wait callback remains
  unclaimed research, not a supported-surface completion criterion.
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
- Ownerless mode rejects platforms or filesystems that fail the
  database-directory mmap, byte-range lock, release-on-death, resize/remap, or
  wait-backend probes, and caches successful proof only for the database
  directory device that was probed.
- Closed-directory copies rebuild stale `.shm` safely through file-identity
  validation, while open-directory copies are rejected or documented unsupported
  until a backup protocol exists.
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
