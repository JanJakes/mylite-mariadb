# Ownerless Native Stress Reopen

## Problem

The native exclusive replay slice proved ordinary `MYLITE_OPEN_READWRITE`
reopen for the explicit commit race and pseudo-random transaction stress shape.
Two existing opt-in stress shapes still had weaker reopen evidence:

- explicit multi-statement transaction/savepoint stress checked only ownerless
  reopen before and after forced `.shm` rebuild;
- checksum stress checked native exclusive reopen, but did not force `.shm`
  rebuild before repeating the oracle.

Those gaps left native exclusive replay less exercised for deterministic
savepoint rollback and mixed direct/prepared checksum writers.

## Source Findings

- MariaDB authority for ownerless transaction behavior remains the repository
  baseline `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), but this slice changes only
  first-party tests and docs.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_transaction_mix_stress()` already computes deterministic
  per-worker sum, weighted-sum, version, and rolled-back-row oracles after
  concurrent explicit transactions.
- The same file's checksum stress already computes deterministic count, sum,
  version, and weighted-sum oracles for mixed direct/prepared writers.
- The native exclusive replay path is implemented by retaining complete
  page-version WAL records after no-live replay, enabling page-version reads for
  ordinary read/write opens, and seeding mapped redo visibility from `.ckpt`.

## Scope And Non-Goals

This slice broadens stress assertions only. It does not change product runtime
code, native checkpoint reclamation, DDL/file-lifecycle replay, public API,
directory layout, dependencies, or binary profile.

## Design

- Make explicit transaction/savepoint stress totals accept open flags.
- Assert explicit transaction/savepoint stress through both ownerless read/write
  and ordinary native exclusive read/write opens before and after forced `.shm`
  rebuild.
- Add forced `.shm` rebuild to checksum stress and repeat both ownerless and
  native exclusive oracles afterward.

## Compatibility Impact

No SQL behavior changes. Compatibility evidence improves for ordinary native
exclusive reopen after ownerless multi-writer stress, specifically for
multi-statement transaction/savepoint and checksum-oracle shapes.

## Test Plan

- Run focused `tx-stress` and `checksum-stress` selectors in the ownerless
  stress build.
- Run the full `ownerless-stress` preset.
- Run formatting and diff checks.

## Acceptance Criteria

- Explicit transaction/savepoint stress passes through ownerless and native
  exclusive reopen before and after forced `.shm` rebuild.
- Checksum stress passes through ownerless and native exclusive reopen before
  and after forced `.shm` rebuild.
- Existing ownerless stress coverage remains green.
