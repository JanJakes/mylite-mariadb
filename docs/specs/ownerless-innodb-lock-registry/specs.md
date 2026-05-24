# Ownerless InnoDB Lock Registry

## Problem

Ownerless transaction IDs and read views make MVCC state visible across mapped
processes, but conflicting writers still rely on InnoDB's process-local lock
manager. InnoDB table locks and record locks are stored as `lock_t` objects that
refer to process-local `trx_t *`, `dict_table_t *`, and lock queues protected by
process-local latches. Cross-process write mode cannot be correct until the
conflict decision for InnoDB table and record locks is represented by
directory-owned state.

This slice adds the first ownerless InnoDB lock-registry primitive. It provides
the shared data model and MariaDB-compatible conflict rules for table locks and
record locks, with fixed-capacity storage and wait/wake behavior. A follow-up
extension adds shared wait-edge publication and wait-cycle detection to the
same registry. It does not yet remove the product exclusive directory lock or
claim product write concurrency.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/lock0types.h` defines table modes
  `LOCK_IS`, `LOCK_IX`, `LOCK_S`, `LOCK_X`, and `LOCK_AUTO_INC`, plus record
  lock flags `LOCK_GAP`, `LOCK_REC_NOT_GAP`, and
  `LOCK_INSERT_INTENTION`.
- `mariadb/storage/innobase/include/lock0priv.h` defines MariaDB's table-lock
  compatibility matrix. The ownerless primitive must preserve those
  compatibility decisions for table-level conflicts.
- `mariadb/storage/innobase/lock/lock0lock.cc` defines process-global
  `lock_sys` and stores lock queues in process memory.
- `lock_rec_has_to_wait()` in `lock0lock.cc` defines the record-lock conflict
  rules: compatible S/S modes do not conflict, ordinary gap locks do not wait,
  record locks do not wait for pure gap locks, gap locks do not wait for
  record-not-gap locks, and no lock waits for an insert-intention lock.
- `lock_rec_lock()` checks process-local explicit record locks and creates
  record lock objects with `lock_rec_create()` or reuses bitmap entries with
  `lock_rec_set_nth_bit()`.
- `lock_rec_dequeue_from_page()`, `lock_rec_discard()`,
  `lock_rec_reset_nth_bit()`, and page split/merge helpers remove or move
  explicit record-lock bits. A hook layer must account for bitmap bit
  add/remove events, not only object allocation.
- `lock_table()` and `lock_table_create()` create table locks after checking the
  process-local table queue; `lock_table_remove_low()` removes them.
- `lock_wait()` waits on per-transaction process-local condition variables.
  Ownerless wait queues must not rely on those condition variables for
  cross-process wait/wake.

## Design

- Add a first-party `ownerless_innodb_lock_registry` shared-memory primitive.
- Store fixed-size entries in `concurrency/mylite-concurrency.shm` through the
  follow-up hook-binding slice. For this primitive slice, tests map it
  directly.
- Each active entry stores:
  - owner process slot ID,
  - MariaDB transaction ID or a MyLite transient lock identity for locks that
    precede `trx_t::id`,
  - table or record kind,
  - table lock mode or record mode/flags,
  - table ID,
  - index ID for record locks,
  - tablespace ID, page number, and heap number for record locks,
  - generation, wait word, and reference count.
- Treat the same owner and same transaction as reentrant for identical lock
  keys. The primitive tracks a reference count so repeated publication can be
  released symmetrically.
- Do not treat the same numeric transaction ID from different owner slots as
  one transaction. MyLite transient lock identities are only unique within the
  owner slot that publishes them.
- Use MariaDB's table compatibility matrix for table conflicts.
- Use the `lock_rec_has_to_wait()` rules for same-record conflicts. Spatial
  predicate locks remain out of scope for this primitive.
- Use the existing MyLite mapped wait word for blocking and waking waiters.
- Store directory-owned wait entries for waiting transactions, with blocker
  owner and transaction identifiers. The registry detects wait cycles before a
  caller sleeps and returns a deadlock result for the new edge.
- Provide owner cleanup so dead process-slot cleanup can remove all lock
  entries and wait entries owned by a process.

## Scope

This slice implements and tests the primitive only. The follow-up hook slice
will publish and release locks from these MariaDB source points:

- table locks: `lock_table_create()` and `lock_table_remove_low()`,
- record lock bit grants: `lock_rec_set_nth_bit()`,
- record lock bit removals: `lock_rec_reset_nth_bit()` and record-lock object
  discard paths,
- lock wait handling: a guarded ownerless wait path before product ownerless
  writers are enabled.

## Non-Goals

- Do not expose `MYLITE_CAP_OWNERLESS_RW`.
- Do not remove the exclusive product directory lock.
- Do not claim SQL-level deadlock-compatible cross-process behavior until the
  InnoDB grant paths publish local waits and consult the registry before grant.
- Do not support spatial predicate locks in the ownerless primitive.
- Do not solve page visibility, redo/checkpoint ownership, or crash recovery in
  this slice.

## Compatibility Impact

No public capability changes. The primitive mirrors MariaDB/InnoDB conflict
rules as groundwork for preserving native lock behavior across processes.

## Directory And Lifecycle Impact

The primitive is now intended to back the `.shm` lock-registry segment. It is
volatile coordination state, not durable truth. Dead-owner cleanup removes all
entries for a process slot.

## Native Storage Impact

No storage files change in this slice. Future hook binding must preserve
InnoDB's native table and record lock semantics, including gap and
insert-intention behavior.

## Test Plan

- Primitive tests for table-lock compatibility:
  - compatible IS/IX holders,
  - same numeric transaction ID from a different owner still conflicts,
  - S blocking IX,
  - X blocking every other table mode,
  - AUTO_INC compatibility according to MariaDB's matrix.
- Primitive tests for record-lock compatibility:
  - S/S compatibility,
  - same numeric transaction ID from a different owner still conflicts,
  - X waiting for S on the same record,
  - gap locks not blocking ordinary record locks,
  - insert-intention locks not blocking other requests,
  - record-not-gap and gap interactions.
- Cross-process wait/wake test where a child waits for a conflicting record
  lock and proceeds when the parent releases it.
- Reentrant reference-count test for identical owner/transaction/lock keys.
- Owner cleanup test.

## Acceptance Criteria

- The primitive stores table and record lock entries in `MAP_SHARED` memory.
- Conflict decisions match the MariaDB source rules covered by tests.
- Waiters block through mapped wait words and wake after release.
- Wait entries are published, cleared on timeout/success/cancel, and checked
  for cross-process cycles.
- Dead-owner cleanup removes table and record locks and wait entries for a
  process slot.
- Ownerless product read/write remains disabled.

## Risks And Unresolved Questions

- Fixed capacity is not sufficient for unlimited ownerless write mode. It must
  become growable before product enablement.
- Record keys based on page number and heap number match InnoDB's explicit lock
  manager, but page split/merge hook coverage must be complete before SQL
  writes can rely on it.
- Cross-process deadlock detection is not solved by this primitive. The hook
  layer needs a directory-owned wait-for graph or a conservative timeout path
  before product writer exposure.
