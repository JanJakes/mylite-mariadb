# Ownerless Release Stabilization

## Problem

The final ownerless release matrix on branch `ownerless-concurrency` proved the
macOS/APFS and Windows/NTFS native gates, but eight Linux shards failed. The
failures cluster around successful or crash-interrupted InnoDB dictionary DDL:

- live-peer table drop can leave an `.ibd` file behind,
- index, primary-key, generated-column foreign-key, and schema-default DDL can
  expose stale rows, stale space identifiers, or a missing tablespace during a
  later open,
- a multi-table rename rollback can leave one expected tablespace absent, and
- an AUTO_INCREMENT crash-recovery marker can remain armed after the final
  live peer exits, and
- sustained peer DDL can leave an already-open DML handle with a stale InnoDB
  target-table cache, surfacing MariaDB errno `1932` on a stable-table
  autocommit `UPDATE`,
- shutdown can durably advance the native checkpoint a few LSNs beyond the
  shared redo recovery anchor, and
- four independent-table autocommit writers can form a physical pre-write
  reservation cycle and surface MariaDB errno `1213` even though no
  transaction has changed a persistent page, and
- a physical-page cycle detected while the InnoDB transaction is still
  `TRX_STATE_NOT_STARTED` can leave the native deadlock-victim marker and
  `DB_DEADLOCK` error state set,
  causing every later InnoDB statement on that connection to return `1213`,
  and
- the explicit-transaction commit-race regression assumed that four
  independent-table workers could never be physical-page deadlock victims, so
  it aborted on a legitimate retryable `1213` instead of retrying the complete
  logical transaction,
- a plain consistent reader could load a native support page without the
  transient preread fence used by writers, observe a checksum-torn page before
  the owning peer completed page-version publication, and fail closed even
  though the matching WAL record became durable immediately afterward, and
- after all peers closed and reclaimed page WAL, a checksum-valid undo page
  could have an LSN covered by the quiescent shared written-redo frontier but
  ahead of the older native redo checkpoint header; startup contained that
  proof but did not activate the bounded page-LSN advance path unless a
  separate retained-WAL or live-peer condition was also present.

Several SQL failures reproduce as isolated selectors, so this is product
lifecycle behavior rather than only weighted-shard ordering or test
contamination. In particular, schema-default recovery attempts to apply retained
page/file-operation state to an `.ibd` file whose completed DDL intentionally
removed it, while primary-key recovery can read a page whose on-disk space
identifier belongs to the replacement tablespace.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB `mariadb/storage/innobase/handler/handler0alter.cc` implements
  rebuilding ALTER operations, including clustered-index replacement, by
  replacing native tablespaces and dictionary identities.
- MariaDB `mariadb/storage/innobase/fil/fil0fil.cc` owns native tablespace
  create, rename, delete, and space-id lookup.
- MariaDB `mariadb/storage/innobase/log/log0recv.cc` replays redo against the
  native tablespace identity recorded by InnoDB and treats a mismatched or
  missing required tablespace as corruption.
- `packages/libmylite/src/database.cc`
  `ownerless_begin_dictionary_ddl()` and
  `ownerless_finish_dictionary_ddl()` publish MyLite dictionary generations
  around MariaDB DDL.
- The same file's
  `mark_ownerless_native_file_op_checkpoint_before_dictionary_finish()` and
  ownerless startup/shutdown recovery paths carry native file-operation
  checkpoint intent across live peers and crashes.
- Ownerless page refresh and native checkpoint proof are keyed by InnoDB
  `(space_id, page_no)`. A rebuilt table must therefore retire stale page
  versions for the old space and make the replacement space authoritative
  before a peer or later opener reuses it.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already contains
  focused selectors for every observed failure and unsafe-hook selectors for
  the crash boundaries. These are the primary compatibility oracles.
- Ownerless mutating statements hold the dictionary statement byte-range lock
  in shared mode, while DDL holds it in exclusive mode. The stress `1932` is
  therefore a post-publication cache-reload failure, not concurrent native DDL
  overlapping the mutation.
- The existing post-error `1932` retry is deliberately restricted to plain
  reads. Retrying a mutation after `mysql_query()` reports an engine error
  would broaden statement replay semantics without proof that every supported
  write shape is side-effect free at that error boundary.
- MariaDB shutdown can publish a later native checkpoint after MyLite's final
  foreground checkpoint update. The shared redo anchor must be seeded before
  every durable ownerless checkpoint update and once more before the shared
  mapping and native hook context are removed.
- Ownerless physical page reservations include clean B-tree pages and
  tablespace gates. A clean reservation can already belong to an open cursor or
  mini-transaction that will modify the page later. A physical reservation
  cycle must therefore abort the current SQL attempt with MariaDB deadlock
  semantics; releasing every transaction reservation and resuming the current
  B-tree operation can let that operation write a page after a peer acquires
  its ownerless reservation.
- Ownerless row undo defers intermediate undo-tail truncation because waiting
  for its history pages while retaining transaction-owned application pages
  can create a physical ownership cycle. The terminal rollback path must
  perform that deferred truncation after restored application pages are
  durable and released, before `commit_empty()` validates the empty undo
  segment.
- The original 4,096-slot shared table/record-lock registry can reach its
  bounded capacity during the 5,000-row transactional performance probe.
  Capacity exhaustion must remain an explicit lock-table-full condition, while
  the release workload needs a versioned layout large enough for its admitted
  transaction size.
- MariaDB's native deadlock-victim marker describes a native lock wait. An
  ownerless cycle discovered before the native transaction starts has no such
  wait to cancel. A cycle discovered during an active operation can also unwind
  to NOT_STARTED before the transaction-end path observes it. Retaining either
  form of native deadlock state violates the idle transaction invariant and
  poisons connection reuse even after SQL `ROLLBACK`.
- Explicit ownerless transactions deliberately expose exact MariaDB `1205` and
  `1213` results rather than replaying a statement inside a transaction whose
  earlier effects may already exist. The safe caller-side unit is the complete
  transaction after rollback; ambiguous commit errno `1180` is not a retry
  signal.
- Native page reads and page-version publication are separate cross-process
  resources. A plain consistent read needs a transient fence only across a
  buffer-pool miss; retaining it for the transaction or advancing the read's
  page-visibility boundary would change MariaDB snapshot semantics.
- With no live process, zero active redo reservation/range state, and
  `latest == written == reserved`, the shared written-redo frontier is
  authoritative evidence for checksum-valid native page LSNs even when the
  native checkpoint header lags and the page-version WAL is already empty.

## Design

Diagnose and repair the shared DDL/file-lifecycle boundary rather than weakening
the tests:

1. Preserve failing directories and inspect checkpoint markers, retained WAL,
   native files, and recorded space identifiers.
2. Reproduce each failed selector independently and, where necessary, minimize
   any weighted prefix that changes the result.
3. Ensure a successful live-peer DDL refresh invalidates or retires page
   versions belonging to replaced/dropped native spaces before they can be
   reapplied to a new or absent tablespace.
4. Keep the native file-operation checkpoint marker armed only while a live
   peer or interrupted DDL still requires no-live native reconciliation.
5. On the last-peer/no-live boundary, complete native checkpoint/recovery and
   clear the marker only after native files and retained page state agree.
6. Preserve crash rollback semantics for multi-table rename and
   AUTO_INCREMENT high-water recovery.
7. When an autocommit DML handle observes a peer dictionary generation, warm
   its parsed target table under the already-held shared dictionary statement
   lock. If that non-mutating probe reaches the stale-engine `1932` boundary,
   run the existing bounded native-page/dictionary repair before the real
   mutation begins. Keep non-stale SQL diagnostics authoritative to the real
   statement and do not retry a mutation after execution starts.
8. Publish the shared redo recovery anchor before any durable checkpoint that
   depends on it, including no-live replacement updates, startup replay, and
   the final native-shutdown checkpoint observed before unmapping.
9. Propagate physical page-reservation deadlocks to the SQL transaction
   boundary even when every reservation is still clean. Stress callers may
   retry the complete transaction after MariaDB `1213`; no path may release
   transaction reservations and resume an already-open cursor or
   mini-transaction.
10. At terminal ownerless rollback, first publish and flush the restored
    application pages, release their transaction page-write ownership, and
    then call `trx_undo_try_truncate()` before entering the native empty-commit
    path. Treat failure of that required cleanup as a coordination fault.
11. Expand the fixed table/record-lock registry to 16,384 slots in a 4 MiB
    minimum `.shm` mapping and increment its segment version. Continue to
    return MariaDB's explicit lock-table-full diagnostic if the new bound is
    exhausted; do not claim unbounded transaction size.
12. Report an ownerless cycle as `DB_DEADLOCK`, but set InnoDB's native
    deadlock-victim marker and native `DB_DEADLOCK` error state only after the
    native transaction has started. A pre-start cycle leaves both clear. If an
    active cycle subsequently returns to NOT_STARTED, successful transaction
    end must prove that no native lock, wait, read view, registration, or
    reference remains, release transient ownerless registrations, and only
    then clear the native result so later statements can reuse the connection.
13. Make the explicit commit-race oracle follow the public retry contract:
    accept only exact `1205`/`1213`, roll back and verify the handle is outside
    a transaction, then retry `START TRANSACTION`, the update, and `COMMIT` as
    one bounded unit. Preserve the one-shot concurrency barrier for the first
    successful update attempt, while allowing a commit victim to retry after
    peers have been released.
14. Admit plain `SELECT` S-latch buffer misses to the existing untracked
    per-page preread fence as a nonblocking probe. On conflict, retry only an
    actually checksum-invalid native read for a small fixed budget; never wait
    behind the peer's transaction-scoped reservation. On successful admission,
    observe the shared redo frontier so a valid peer page is not rejected as
    future, and restore no writer-only wait state. Do not push raw-latest page
    visibility over the statement's selected snapshot.
15. Activate no-live startup page-LSN advancement when a valid current redo
    prefix, a nonzero durable ownerless checkpoint, and a quiescent shared redo
    snapshot prove the written frontier. Keep fail-closed behavior when any
    active reservation/range remains or the three shared frontiers differ.
16. Preserve a newer structural DDL marker when the final survivor predates
    the latest process generation and its file-per-table identity snapshot has
    changed. This deferral must not depend on user page-version WAL still being
    present: foreground or scheduler checkpointing may compact those user
    records before the older survivor closes, but it does not make that
    survivor's native dictionary authoritative for the newer lifecycle. The
    following isolated latest-generation owner must likewise recognize retained
    native-support-only payload as the structural handoff, replay and prove the
    surviving native tablespaces, retire only the file-operation marker, and
    leave any independent DML/history obligation intact.

The implementation must continue to fail closed when required native state is
missing or cannot be reconciled. It must not convert corruption into a retry,
silently ignore unsupported filesystems, or relax row/file assertions.

## Scope

In scope:

- all eight final-matrix Linux failures,
- live-peer DDL dictionary and tablespace replacement/drop refresh,
- DDL crash-marker cleanup at last-peer/no-live recovery,
- multi-rename rollback file identity,
- deterministic regression tests or diagnostics needed to prove the fix,
- recovery-anchor ordering at the native shutdown boundary,
- transaction-boundary handling for clean pre-write reservation cycles without
  resuming an invalidated B-tree operation,
- bounded whole-transaction retry in the concurrent commit-race compatibility
  oracle,
- transient plain-read coordination for native buffer-pool misses,
- no-live startup from a quiescent shared written-redo frontier after page WAL
  reclamation,
- multi-page ownerless rollback terminal cleanup and connection reuse,
- a versioned lock-registry capacity sufficient for the 5,000-row release
  transaction while preserving explicit bounded-capacity failure,
- compatibility and roadmap notes if the supported behavior changes.

Out of scope:

- new SQL compatibility surfaces unrelated to the failing selectors,
- a new durable file format unless existing marker/WAL semantics cannot express
  the required invariant,
- daemon or wire-protocol work,
- cross-process behavior on filesystems rejected by the explicit platform and
  filesystem qualification gate.

## Compatibility, Directory, And Native Storage Impact

The intended SQL behavior remains MariaDB-compatible. The change strengthens
ownerless durability and live-peer visibility for already-supported InnoDB DDL.

No durable state may escape the MyLite-owned directory. Existing native
MariaDB/InnoDB files remain in native format. If durable marker interpretation
changes, it must remain backward-compatible with directories created by the
current branch and be covered by reopen tests.

## Public API, Build, Size, License, And Dependencies

No public API, license, or dependency change is intended. Production build
profiles and platform qualification must remain green. Any binary-size
movement must be measured by the existing release gates.

## Test Plan

- Re-run all eight exact failed selectors or hook shards in the hook-enabled
  build.
- Run every focused SQL selector that failed in the 16-way production matrix:
  idempotent table DDL, generated-column ALTER, index DDL, generated-column
  foreign keys, schema-default DDL, and primary-key DDL.
- Run the multi-rename rollback and crashed AUTO_INCREMENT dictionary DDL hook
  selectors.
- Run adjacent DDL, recovery, page-log, native-checkpoint, and crash-tail
  coverage.
- Run all 16 production ownerless SQL shards and the complete hook release
  gate locally from clean temporary directories, then run their independent
  native CI jobs again on the pushed final commit.
- Repeat the checkpoint-anchor commit race and run true row, gap-lock, and
  independent-table deadlock coverage around the physical-cycle repair. Repeat
  the commit race enough times to exercise a physical-page victim and require
  all four logical commits exactly once after ownerless/native reopen and
  forced shared-memory rebuild.
- Repeat the four-process mixed workload with three autocommit writers and a
  plain reader. Every read total remains monotonic and bounded, no native page
  checksum failure is accepted, and both ordinary native and ownerless reopens
  observe the exact final total after all peers close.
- Repeat the pseudo-random same-table savepoint schedule that exposed the
  pre-start victim-marker leak, and require the deterministic two-process
  deadlock victim to execute an InnoDB locking read in a fresh transaction on
  the same connection before exiting.
- Run a focused ownerless transaction that creates a multi-page update-undo
  tail, covers both full rollback and rollback-to-savepoint followed by commit,
  reuses the same connection, and reopens natively.
- Run the exact 5,000-row transactional performance probe against the expanded
  lock registry and verify the generated `.shm` layout through the embedded
  open/close tests.
- Run the full local production/native CI gates, format/tidy checks,
  `git diff --check`, and the macOS/APFS and Windows/NTFS native jobs after
  pushing the fix.

## Acceptance Criteria

- Every one of the eight final-matrix failures passes without retries or
  weakened assertions.
- Dropped/replaced tablespaces have the expected final `.frm`/`.ibd` presence,
  space identifier, dictionary metadata, and rows through live-peer, ownerless,
  native, and forced-`.shm` rebuild opens.
- Retained ownerless page state is never replayed into a different native
  space identity.
- Missing files caused by completed DROP/replacement DDL are not treated as
  required recovery inputs.
- Sustained DDL/DML stress does not expose errno `1932` on the stable DML table,
  and the fix does not classify mutating statements as post-error retryable.
- A clean native shutdown never leaves the durable checkpoint visibility ahead
  of the shared redo recovery anchor.
- Clean pre-write cycles surface MariaDB errno `1213` at the SQL transaction
  boundary, while bounded stress callers retry the complete transaction and
  conflicting row and gap-lock transactions retain MariaDB deadlock behavior.
- The explicit commit race accepts only `1205`/`1213` retry outcomes, proves
  rollback leaves the same handle reusable, retries the complete logical
  transaction through `COMMIT`, and preserves every worker delta exactly once.
- Plain consistent buffer-miss reads never wait behind a peer transaction,
  preserve their selected snapshot, and retry only a checksum-invalid native
  read that raced physical page or page-version publication.
- No-live startup accepts a checksum-valid native page ahead of the native redo
  checkpoint only when the quiescent shared written-redo frontier and durable
  ownerless checkpoint prove that exact upper bound.
- A pre-start ownerless deadlock never sets a native victim marker or
  `DB_DEADLOCK` error state, and successful transaction end safely clears
  either value left by an active operation that has returned to NOT_STARTED;
  explicit rollback followed by another InnoDB transaction succeeds without a
  repeated synthetic `1213`.
- Multi-page ownerless rollback truncates its empty native undo tail before
  native empty-commit validation and leaves the connection reusable.
- The release performance transaction completes inside the 16,384-slot
  version-6 record-lock registry; exhaustion beyond that bound remains an
  explicit lock-table-full error.
- Interrupted DDL retains the native file-operation checkpoint marker while a
  live peer still requires it. A final older survivor that observed a changed
  file-per-table identity set retains the marker even if user page-version WAL
  was already checkpointed; an isolated latest-generation recovery clears it
  from either ordinary-user or native-support-only retained payload.
- Multi-table rename rollback restores both native tablespaces and dictionary
  identities.
- The full Linux ownerless matrix, macOS/APFS gate, and Windows/NTFS gate pass
  on the final commit.

## Risks And Follow-Up

- The observed failures span both successful live-peer refresh and
  crash-interrupted recovery, so an overly broad marker change could fix one
  class while regressing the other.
- Native checkpoint scheduling must not discard page-version WAL without the
  existing exact native proof.
- A repaired bounded matrix is release evidence, not a claim that every
  possible concurrent DDL interleaving has been exhaustively explored.

## Results

The stabilization is complete for the admitted ownerless surface:

- completed DROP and replacement DDL no longer replays retained state into an
  absent or different tablespace identity,
- the native file-operation marker stays armed across live-peer recovery and
  drains only after the final no-live reconciliation,
- exact-generation preflight repairs stale autocommit DML dictionary state
  before mutation without introducing post-execution write replay,
- every durable ownerless checkpoint is preceded by a shared redo-anchor
  publication, including the final native shutdown checkpoint,
- page-write reservation deadlocks abort the current SQL attempt instead of
  releasing every transaction-owned page and resuming an already-open
  mini-transaction; the bounded stress callers retry the complete transaction,
  preserving MariaDB deadlock semantics without an unlocked page mutation, and
- terminal ownerless rollback truncates deferred multi-page native undo only
  after restored application-page ownership is durable and released,
- terminal rollback records a page-scoped native-read barrier at each restored
  user page's prior committed boundary, retains that barrier through page-index
  rebuild/checkpoint replacement, and preserves the handle's monotonic
  pre-transaction committed read boundary,
- the version-6 shared table/record-lock registry provides 16,384 slots in the
  4 MiB minimum volatile `.shm` layout, with explicit bounded-capacity failure
  retained, and
- the concurrent foreign-key graph gate retries the complete transaction when
  MariaDB reports native `1205`/`1213` contention at `COMMIT`, matching its
  existing per-statement retry contract, serializes only post-conflict final
  delete retries, and proves a forced cascade-parent full rollback against
  exact parent/child values, and
- the concurrent DDL/DML stress gate applies that same bounded native
  `1205`/`1213` contract to its autocommit statements and read-only polls, in
  addition to pre-execution MyLite statement-lock contention, and
- peer-page and dictionary refresh now fail closed if required page
  materialization cannot be completed,
- plain consistent native page loads use a nonblocking publication fence plus
  checksum-triggered bounded reread without widening snapshot visibility, and
- no-live startup can use the quiescent shared written-redo frontier after page
  WAL reclamation instead of rejecting a checksum-valid native support page
  solely because the native checkpoint header is older, and
- final older survivors retain newer structural DDL recovery through
  file-per-table lifecycle changes independently of whether user page-version
  WAL happened to remain at shutdown, and isolated latest-generation recovery
  drains that handoff even when checkpoint scheduling has already reduced it to
  native-support-only history.

Final-source evidence passes `65/65` ordinary production tests, `2/2` PHP
ownerless adapter tests, all `16` weighted SQL shards, all `270` hook release
tests across sixteen deterministic CI shards, the `5/5` workload and `5/5`
randomized presets, the `4/4` pressure preset, mounted
ext4/XFS/tmpfs/overlay qualification, explicit unsupported-filesystem
rejection, focused WordPress `7/7` with `22` assertions and `76` peer writes,
the `12/12` MariaDB trace oracle, and `32` seeds each for random transactions,
DDL, and FK graphs. Native CI supplies the corresponding final macOS/APFS and
Windows/NTFS proof. Format, clang-tidy, build-policy, build-type, whitespace,
and bundle-size gates also pass.

The pinned WordPress source requires the test-only
`wp-coding-standards/wpcs 3.3.0` package. Composer's later advisory-blocking
default rejects that exact package for advisory `PKSA-mh9b-91zm-m1gy`, before
the ownerless gate can run. The harness records an exact-ID exception for that
pinned coding-standard dependency while leaving Composer's broader advisory
blocking enabled; the CI production-build audit requires the narrow exception.
