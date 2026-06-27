# Ownerless Foreign-Key Truncate Variants Live Recovery

## Problem Statement

Ownerless live truncate recovery already covers plain, implicit, and
child-only foreign-key `TRUNCATE TABLE` boundaries. The remaining positive
foreign-key truncate shapes include MariaDB-supported self-referencing
foreign keys and generated-column child foreign keys. MariaDB rejects a
non-self-referencing parent-table truncate before InnoDB truncates the table,
so that path is a negative compatibility behavior rather than a live recovery
boundary.

MyLite needs focused hook evidence that supported FK truncate variants that
complete inside MariaDB before `dictionary-before-finish` can be recovered
while another ownerless peer remains live, preserving FK metadata,
generated-column metadata, marker retention/drain, forced `.shm` rebuild, and
ordinary native reopen.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_truncate.cc:fk_truncate_illegal_if_parent()` rejects
  truncating a table that is referenced by a non-self-referencing FK, but
  allows self-referencing FK tables to proceed.
- `mariadb/sql/sql_truncate.cc:Sql_cmd_truncate_table::handler_truncate()`
  performs that FK parent check before `handler::ha_truncate()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:ha_innobase::truncate()`
  depends on the SQL-layer parent-FK check before native truncate.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_table_info_t::create_foreign_keys()`
  accepts generated-column FK metadata after validating indexes, generated
  column restrictions, and referenced metadata.
- `packages/libmylite/src/database.cc:ownerless_truncate_table_recovery_statement()`
  already classifies focused `TRUNCATE [TABLE] schema.table` statements as
  recoverable dictionary DDL.
- `packages/libmylite/src/database.cc:ownerless_process_recover_dead_dictionary_owner()`
  treats `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE` as a native
  file-operation recovery kind only while the durable native file-operation
  checkpoint-needed marker is set.

## Scope And Non-Goals

In scope:

- Add one hook selector,
  `dictionary-foreign-key-truncate-variants-crash`.
- Kill a self-referencing FK table truncate at `dictionary-before-finish` and
  verify live recovery, empty table state, FK metadata, post-recovery
  enforcement, marker retention/drain, forced `.shm` rebuild, and ordinary
  native reopen.
- Kill a generated-column child FK table truncate at
  `dictionary-before-finish` and verify generated-column metadata, FK
  metadata, generated value recomputation, post-recovery enforcement, marker
  retention/drain, forced `.shm` rebuild, and ordinary native reopen.
- Keep the existing truncate classifier unchanged unless focused verification
  proves it is too narrow.

Out of scope:

- Positive parent-table truncate recovery; MariaDB rejects that before native
  truncate when the parent is referenced by another table.
- Partition truncate; ownerless read/write mode rejects partitioned and
  partition-maintenance DDL before native file lifecycle.
- Cyclic multi-table FK truncate under default FK checks; MariaDB rejects that
  before native truncate, and
  `docs/specs/ownerless-cyclic-foreign-key-truncate-negative-proof/specs.md`
  covers it as a negative proof. The `FOREIGN_KEY_CHECKS=0` positive native
  truncate path is covered separately by
  `docs/specs/ownerless-unchecked-cyclic-foreign-key-truncate-live-recovery/specs.md`.
- Temporary-table truncate and randomized external MariaDB/RQG coverage.

## Design

The self-referencing variant uses:

```sql
CREATE TABLE app.ownerless_fk_truncate_self (
  id INT NOT NULL PRIMARY KEY,
  parent_id INT NULL,
  value INT NOT NULL,
  INDEX ownerless_fk_truncate_self_parent_idx (parent_id),
  CONSTRAINT ownerless_fk_truncate_self_parent
    FOREIGN KEY (parent_id)
    REFERENCES app.ownerless_fk_truncate_self (id)
) ENGINE=InnoDB;
```

The generated-column child variant uses:

```sql
CREATE TABLE app.ownerless_fk_truncate_generated_parent (
  id INT NOT NULL PRIMARY KEY,
  value INT NOT NULL
) ENGINE=InnoDB;

CREATE TABLE app.ownerless_fk_truncate_generated_child (
  id INT NOT NULL PRIMARY KEY,
  raw_parent INT NOT NULL,
  parent_key INT GENERATED ALWAYS AS (raw_parent + 100) STORED,
  value INT NOT NULL,
  INDEX ownerless_fk_truncate_generated_child_idx (parent_key),
  CONSTRAINT ownerless_fk_truncate_generated_child_parent
    FOREIGN KEY (parent_key)
    REFERENCES app.ownerless_fk_truncate_generated_parent (id)
) ENGINE=InnoDB;
```

Each subcase keeps a live ownerless peer open, kills a writer at
`dictionary-before-finish` while the writer runs the targeted `TRUNCATE TABLE`,
opens another ownerless handle to recover the dead dictionary generation, and
checks that the native file-operation marker remains durable while the
original peer is live. After releasing the peer, no-live recovery drains the
marker and the recovered state is checked through ownerless/native reopen
before and after forced `.shm` rebuild.

## Compatibility Impact

This expands ownerless FK truncate evidence to two additional
MariaDB-supported truncate shapes. It does not broaden MyLite SQL grammar or
partition support, and it does not claim parent-table truncate recovery where
MariaDB rejects the statement.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. The slice relies on
MariaDB/InnoDB native truncate behavior and MyLite's existing durable
native file-operation checkpoint-needed marker inside the MyLite database
directory.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The
change adds hook-only tests, a CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run:
  `mylite_ownerless_cross_process_sql_test dictionary-foreign-key-truncate-variants-crash`.
- Run the registered focused CTest, including repeated runs.
- Run adjacent truncate/FK hook CTests and ownerless primitives.
- Build the same test in `php-embedded-prod` and run relevant production
  selectors for generated-column FK, cyclic FK, FK DDL/actions, and truncate
  replay.
- Run ownerless DDL stress, production-build guard, format check, and
  `git diff --check`.

## Acceptance Criteria

- Both subcases kill the writer at `dictionary-before-finish`.
- A live ownerless opener recovers the dead truncate generation while another
  peer remains open.
- Both target tables are present and empty immediately after recovery.
- Self-referencing FK metadata remains present and rejects missing-parent
  inserts plus referenced-parent deletes after a valid child row is inserted.
- Generated-column child FK metadata and generated-column metadata remain
  present; generated values recompute correctly; missing-parent inserts and
  referenced-parent deletes remain rejected after a valid child row is
  inserted.
- The native file-operation marker remains set while the original live peer is
  open and drains after final no-live recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild preserve the
  recovered rows, generated values, and FK enforcement.

## Verification Results

Completed on 2026-06-27 with `ownerless-test-hooks`, `ownerless-stress`, and
`php-embedded-prod` production builds:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-foreign-key-truncate-variants-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-foreign-key-truncate-variants-crash$' --repeat until-fail:5 --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-truncate-file-op-marker-crash$|^libmylite\.ownerless-dictionary-implicit-truncate-crash$|^libmylite\.ownerless-dictionary-foreign-key-child-truncate-crash$|^libmylite\.ownerless-dictionary-foreign-key-truncate-variants-crash$|^libmylite\.ownerless-dictionary-foreign-key-crash$|^libmylite\.ownerless-dictionary-foreign-key-drop-crash$|^libmylite\.ownerless-primitives$|^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- Production direct selectors passed before the final format-only rewrite:
  `truncated-tablespace-replay`, `foreign-key-ddl`, `foreign-key-actions`,
  `foreign-key-deep-cascade`, `generated-column-foreign-key`,
  `cyclic-foreign-key`, `cyclic-foreign-key-variants`, and
  `foreign-key-cross-schema-multi-rename`.
- After formatting and rebuilding the production target,
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test generated-column-foreign-key`
  passed.
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `git diff --check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `env LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset prod --target format-check`

## Risks And Follow-Up

- Cyclic multi-table FK truncate is covered by
  `docs/specs/ownerless-cyclic-foreign-key-truncate-negative-proof/specs.md`
  as a MariaDB pre-truncate error path under default FK checks, and the
  unchecked positive recovery path is covered by
  `docs/specs/ownerless-unchecked-cyclic-foreign-key-truncate-live-recovery/specs.md`.
- Temporary-table truncate remains a separate follow-up.
- Parent-table truncate remains a MariaDB error path; additional negative
  coverage can be added if the compatibility matrix needs a direct ownerless
  assertion for errno stability.
- Broader native redo/checkpoint reconciliation, active-reader pressure crash
  breadth, and external MariaDB/RQG stress remain open completion gates.
