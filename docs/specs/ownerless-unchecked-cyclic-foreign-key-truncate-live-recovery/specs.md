# Ownerless Unchecked Cyclic Foreign-Key Truncate Live Recovery

## Problem Statement

Ownerless cyclic foreign-key truncate coverage currently proves the default
MariaDB behavior: `TRUNCATE TABLE` on either table in a multi-table FK cycle
fails before native truncate when `FOREIGN_KEY_CHECKS=1`. That is not the only
MariaDB-compatible path. When a session disables foreign-key checks, MariaDB
skips the parent-FK truncate rejection and can reach the native
truncate/recreate path.

MyLite needs focused hook evidence that this reachable unchecked cyclic FK
truncate boundary recovers while another ownerless peer remains live, preserves
metadata and the intentionally orphaned peer row, keeps the native file-op
marker durable until no-live recovery, drains it after peer release, and
survives ownerless reopen, ordinary native reopen, and forced `.shm` rebuild.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_truncate.cc:120` implements
  `fk_truncate_illegal_if_parent()`, which emits
  `ER_TRUNCATE_ILLEGAL_FK` when the target table is referenced by a
  non-self-referencing foreign key.
- `mariadb/sql/sql_truncate.cc:231-233` calls that parent-FK check only when
  `OPTION_NO_FOREIGN_KEY_CHECKS` is clear. A session with
  `FOREIGN_KEY_CHECKS=0` skips the default cyclic-FK rejection.
- `mariadb/sql/sql_truncate.cc:510-532` reaches `dd_recreate_table()` or
  `handler_truncate()` after table locking and preflight checks succeed.
- Existing ownerless FK truncate recovery already covers child-only,
  self-referencing, and generated-column child truncate boundaries at
  `dictionary-before-finish`.
- `docs/specs/ownerless-cyclic-foreign-key-truncate-negative-proof/specs.md`
  covers the default-checked cyclic FK truncate path as a negative proof, not a
  positive recovery boundary.

## Scope And Non-Goals

In scope:

- Add hook selector
  `dictionary-unchecked-cyclic-foreign-key-truncate-crash`.
- Create a two-table cyclic InnoDB FK graph under ownerless read/write mode.
- Run `SET SESSION foreign_key_checks = 0` and kill a writer at
  `dictionary-before-finish` while truncating one table in the cycle.
- Verify live ownerless recovery leaves the truncated table empty, preserves
  the peer table row that now intentionally references a missing parent,
  preserves both FK metadata records, and preserves enforcement for new
  checked writes after the session returns to default FK checks.
- Verify the native file-operation checkpoint-needed marker remains set while
  a live ownerless peer is open and drains after final no-live recovery.
- Verify ownerless reopen, ordinary native reopen, and forced `.shm` rebuild
  preserve the restored valid cycle and FK enforcement.

Out of scope:

- Default `FOREIGN_KEY_CHECKS=1` cyclic FK truncate; that path is the existing
  negative proof.
- Parent-table truncate under default FK checks; MariaDB rejects it before
  native truncate.
- Partition or temporary-table truncate behavior.
- Broader unchecked DDL matrices and randomized external MariaDB/RQG coverage.

## Design

The slice adds one hook-only test case around this cyclic graph:

```sql
CREATE TABLE app.ownerless_fk_truncate_unchecked_a (
  id INT NOT NULL PRIMARY KEY,
  b_id INT NULL,
  value INT NOT NULL,
  INDEX ownerless_fk_truncate_unchecked_a_b_idx (b_id)
) ENGINE=InnoDB;

CREATE TABLE app.ownerless_fk_truncate_unchecked_b (
  id INT NOT NULL PRIMARY KEY,
  a_id INT NOT NULL,
  value INT NOT NULL,
  INDEX ownerless_fk_truncate_unchecked_b_a_idx (a_id),
  CONSTRAINT ownerless_fk_truncate_unchecked_b_a
    FOREIGN KEY (a_id)
    REFERENCES app.ownerless_fk_truncate_unchecked_a (id)
    ON UPDATE RESTRICT
    ON DELETE RESTRICT
) ENGINE=InnoDB;

ALTER TABLE app.ownerless_fk_truncate_unchecked_a
  ADD CONSTRAINT ownerless_fk_truncate_unchecked_a_b
  FOREIGN KEY (b_id)
  REFERENCES app.ownerless_fk_truncate_unchecked_b (id)
  ON UPDATE RESTRICT
  ON DELETE RESTRICT;
```

The test inserts one valid cycle, keeps a live ownerless peer open, then kills
a writer at `dictionary-before-finish` while it runs:

```sql
SET SESSION foreign_key_checks = 0;
TRUNCATE TABLE app.ownerless_fk_truncate_unchecked_a;
```

Recovery should observe table `a` empty and table `b` retaining its row. With
default FK checks restored on the recovery handle, inserting a `b` row that
references a missing `a` row must fail. Re-inserting `a.id=1` with `b_id=1`
restores a valid cycle, after which deleting either referenced row must fail
until the dependent row is removed.

No product classifier change is expected. If focused coverage shows MyLite
cannot recover this native truncate path, the recovery logic rather than the
test expectation must be fixed.

## Compatibility Impact

This expands ownerless compatibility evidence for a MariaDB-supported
`FOREIGN_KEY_CHECKS=0` truncate shape. It does not make unchecked FK use safer
than MariaDB: the peer row can become orphaned by design while checks are
disabled, and later checked writes must still enforce FK metadata.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. The slice relies on
MariaDB/InnoDB native truncate behavior and MyLite's existing durable native
file-operation checkpoint-needed marker inside the MyLite database directory.
The test specifically proves live-peer marker retention and no-live drain for
this unchecked cyclic FK native file lifecycle.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The
change adds hook-only test coverage, a CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run:
  `mylite_ownerless_cross_process_sql_test dictionary-unchecked-cyclic-foreign-key-truncate-crash`.
- Run the registered focused CTest, including adjacent FK truncate recovery
  CTests.
- Build the same test in `php-embedded-prod` and run adjacent production
  selectors:
  `cyclic-foreign-key`, `cyclic-foreign-key-variants`, and
  `cyclic-foreign-key-truncate-policy`.
- Run ownerless DDL stress smoke, production-build guard, format check, and
  `git diff --check`.

## Acceptance Criteria

- The killed writer reaches the `dictionary-before-finish` hook after
  `FOREIGN_KEY_CHECKS=0` allows native truncate to proceed.
- A live ownerless opener recovers the dead truncate generation while another
  peer remains open.
- The truncated table is present and empty; the peer table keeps its orphaned
  row until the test restores a valid cycle.
- Both FK metadata records remain present in `information_schema`.
- Checked writes after recovery reject missing-parent inserts and referenced
  deletes.
- The native file-operation marker remains set while the original live peer is
  open and drains after final no-live recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild preserve the
  restored cycle and FK enforcement.

## Verification Results

Completed on 2026-06-27 with `ownerless-test-hooks`, `php-embedded-prod`,
`ownerless-stress`, and `prod` builds:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-unchecked-cyclic-foreign-key-truncate-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-unchecked-cyclic-foreign-key-truncate-crash$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-(foreign-key-child-truncate-crash|foreign-key-truncate-variants-crash|unchecked-cyclic-foreign-key-truncate-crash)$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- Production adjacent selectors:
  `cyclic-foreign-key`, `cyclic-foreign-key-variants`, and
  `cyclic-foreign-key-truncate-policy`.
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cyclic-foreign-key-truncate-policy$' --output-on-failure`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed on exact rerun. The first run hit an intermittent InnoDB startup
  `Data structure corruption` abort in a generated
  `/tmp/mylite-ownerless-sql.*` stress directory; no ownerless test process was
  left behind, the generated temp directory was removed, and the exact rerun
  passed.
- Guards:
  `tools/check-ci-production-builds`,
  `tools.ci-production-builds`, `format-check`, and `git diff --check`.
- Final focused selector rerun after formatting:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-unchecked-cyclic-foreign-key-truncate-crash`.

## Risks And Follow-Up

- This is one bounded `FOREIGN_KEY_CHECKS=0` cyclic truncate shape, not broad
  unchecked DDL coverage.
- Temporary-table truncate and broader DDL file-lifecycle recovery remain open.
- Broader native redo/checkpoint reconciliation, active-reader pressure crash
  breadth, and external MariaDB/RQG stress remain ownerless completion gates.
