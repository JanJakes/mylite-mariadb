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
  transaction has changed a persistent page.

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
  tablespace gates. A cycle among only clean pre-write reservations is safe to
  break by releasing the selected transaction's reservations and restarting
  its B-tree search. Once the transaction has undo or a dirty ownerless page,
  the cycle is a real SQL deadlock and must retain MariaDB's victim semantics.

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
9. If the physical page reservation layer selects a transaction that has
   neither undo nor dirty ownerless pages as a deadlock victim, release its
   clean page reservations and tablespace gates and retry the reservation.
   Preserve the deadlock result for transactions that have begun a persistent
   change.

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
- clean pre-write reservation-cycle handling without weakening real
  transaction deadlock detection,
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
  independent-table deadlock coverage around the physical-cycle repair.
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
- Independent-table clean pre-write cycles restart without surfacing errno
  `1213`, while conflicting row and gap-lock transactions retain MariaDB
  deadlock behavior.
- Interrupted DDL retains the native file-operation checkpoint marker while a
  live peer still requires it, and the final no-live recovery clears it.
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
- clean pre-write physical reservation cycles restart inside the original lock
  wait budget, while transactions with undo or dirty pages preserve MariaDB
  deadlock semantics, and
- peer-page and dictionary refresh now fail closed if required page
  materialization cannot be completed.

Final-source evidence passes `65/65` ordinary production tests, `2/2` PHP
ownerless adapter tests, all `16` weighted SQL shards, all `269` hook release
tests, the `5/5` workload and `5/5` randomized presets, the `4/4` pressure
preset, mounted ext4/XFS/tmpfs/overlay qualification, explicit unsupported
filesystem rejection, focused WordPress `7/7` with `22` assertions and `71`
peer writes, the `12/12` MariaDB trace oracle, and `32` seeds each for random
transactions, DDL, and FK graphs. Native CI supplies the corresponding final
macOS/APFS and Windows/NTFS proof. Format, clang-tidy, build-policy, build-type,
whitespace, and bundle-size gates also pass.
