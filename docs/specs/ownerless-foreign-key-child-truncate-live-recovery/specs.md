# Ownerless Foreign-Key Child Truncate Live Recovery

## Problem Statement

Ownerless live truncate recovery proves focused plain and implicit
non-temporary `TRUNCATE TABLE` boundaries. The remaining truncate gap includes
foreign-key variants. MariaDB rejects truncating a non-self-referencing parent
table, but allows a table that is only a child in a foreign-key relationship to
proceed through the normal truncate path.

MyLite needs bounded evidence that a child-table truncate completed by MariaDB
before `dictionary-before-finish` can be recovered while another ownerless peer
remains live, preserving parent rows, child-table emptiness, foreign-key
metadata, enforcement, native file-operation marker retention/drain, forced
`.shm` rebuild, and ordinary native reopen.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_truncate.cc:fk_truncate_illegal_if_parent()` rejects
  truncate only when the target table is the parent in a non-self-referencing
  foreign key; a child-only table can continue.
- `mariadb/sql/sql_truncate.cc:Sql_cmd_truncate_table::handler_truncate()`
  checks the foreign-key parent rule before calling `handler::ha_truncate()`.
- `mariadb/sql/sql_truncate.cc:Sql_cmd_truncate_table::truncate_table()` can
  use `dd_recreate_table()` for engines with `HTON_CAN_RECREATE`, otherwise
  it uses the handler truncate path.
- `mariadb/storage/innobase/handler/ha_innodb.cc:ha_innobase::truncate()`
  relies on the SQL-layer foreign-key parent check before truncating the
  native InnoDB table.
- Existing MyLite
  `packages/libmylite/src/database.cc:ownerless_truncate_table_recovery_statement()`
  already classifies the deterministic `TRUNCATE TABLE schema.table` shape as
  recoverable dictionary DDL.

## Scope And Non-Goals

In scope:

- Add a hook selector,
  `dictionary-foreign-key-child-truncate-crash`, for
  `TRUNCATE TABLE app.ownerless_fk_truncate_child`.
- Keep the existing truncate classifier unchanged.
- Verify live-peer recovery while another ownerless peer remains open.
- Verify parent rows remain present, the child table is empty immediately
  after recovery, and the recovered child foreign-key metadata is present.
- Verify valid child inserts still enforce parent existence and parent deletes
  are rejected while a recovered child row references the parent.
- Verify native file-operation marker retention while a peer is live and drain
  after final no-live recovery.

Out of scope:

- Parent-table truncate rejection beyond relying on MariaDB's native error.
- Self-referencing FK truncate, cyclic FK graphs, generated-column FKs,
  partition truncate, temporary-table truncate, `ALTER TABLE ... TRUNCATE
  PARTITION`, and external randomized DDL/RQG oracles.

## Design

The test creates:

```sql
CREATE TABLE app.ownerless_fk_truncate_parent (
  id INT NOT NULL PRIMARY KEY,
  value INT NOT NULL
) ENGINE=InnoDB;

CREATE TABLE app.ownerless_fk_truncate_child (
  id INT NOT NULL PRIMARY KEY,
  parent_id INT NOT NULL,
  value INT NOT NULL,
  INDEX ownerless_fk_truncate_child_parent_idx (parent_id),
  CONSTRAINT ownerless_fk_truncate_child_parent
    FOREIGN KEY (parent_id)
    REFERENCES app.ownerless_fk_truncate_parent (id)
) ENGINE=InnoDB;
```

It inserts two parent rows and two child rows, keeps a live ownerless peer open,
then kills a writer at `dictionary-before-finish` while the writer runs:

```sql
TRUNCATE TABLE app.ownerless_fk_truncate_child
```

Recovery opens another ownerless handle, finishes the dead dictionary
generation, verifies the child table is empty and the foreign-key metadata is
still visible, inserts one valid child row, rejects one missing-parent child
insert, rejects deleting the referenced parent row, and verifies marker
retention. After releasing the original peer, no-live recovery drains the
marker and the same parent/child/FK state is verified through ownerless/native
reopen before and after forced `.shm` rebuild.

## Compatibility Impact

This expands ownerless truncate evidence to a MariaDB-supported child-table
foreign-key shape. It does not broaden MyLite's supported grammar beyond the
existing focused `TRUNCATE TABLE schema.table` recovery shape.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. The slice relies on
MariaDB/InnoDB's native truncate behavior and the existing MyLite durable
native file-operation checkpoint marker inside the MyLite database directory.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size changes. The
change adds hook-only test coverage, CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused selector:
  `mylite_ownerless_cross_process_sql_test dictionary-foreign-key-child-truncate-crash`.
- Run the registered focused CTest repeatedly.
- Run adjacent hook CTests for truncate, FK add/drop crash recovery,
  ownerless primitives, and embedded ownerless InnoDB lock hooks.
- Build `mylite_ownerless_cross_process_sql_test` in `php-embedded-prod`.
- Run production selectors for `truncated-tablespace-replay`,
  `foreign-key-ddl`, `foreign-key-actions`, and `foreign-key-deep-cascade`.
- Run ownerless DDL stress, the production embedded ownerless SQL subset,
  format/production-build guards, and `git diff --check`.

## Acceptance Criteria

- The focused hook selector kills the writer at `dictionary-before-finish`.
- A live ownerless opener recovers the dead child-truncate dictionary
  generation while another ownerless peer remains open.
- The child table is present and empty immediately after recovery.
- The recovered foreign-key metadata remains visible in
  `information_schema.referential_constraints` and
  `information_schema.key_column_usage`.
- A valid child insert succeeds, a missing-parent child insert fails with
  MariaDB errno `1452`, and deleting the referenced parent fails with MariaDB
  errno `1451`.
- The native file-operation marker remains set while the original live peer is
  open and drains after final no-live recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild preserve the
  parent rows, recovered child row, and foreign-key enforcement.

## Verification Results

Completed on 2026-06-27 with `ownerless-test-hooks`, `ownerless-stress`, and
`php-embedded-prod` production builds:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-foreign-key-child-truncate-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-foreign-key-child-truncate-crash$' --repeat until-fail:5 --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-truncate-file-op-marker-crash$|^libmylite\.ownerless-dictionary-implicit-truncate-crash$|^libmylite\.ownerless-dictionary-foreign-key-child-truncate-crash$|^libmylite\.ownerless-dictionary-foreign-key-crash$|^libmylite\.ownerless-dictionary-foreign-key-drop-crash$|^libmylite\.ownerless-primitives$|^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- Production direct selectors passed: `truncated-tablespace-replay`,
  `foreign-key-ddl`, `foreign-key-actions`, `foreign-key-deep-cascade`,
  `cyclic-foreign-key`, and `foreign-key-cross-schema-multi-rename`.
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.' --parallel 1 --output-on-failure`

## Risks And Follow-Up

- Self-referencing FK truncate and stored generated-column child FK truncate
  are covered by
  `docs/specs/ownerless-foreign-key-truncate-variants-live-recovery/specs.md`.
- Parent-table truncate rejection remains a MariaDB pre-truncate error path.
  Cyclic FK truncate is covered by
  `docs/specs/ownerless-cyclic-foreign-key-truncate-negative-proof/specs.md`
  as the same class of pre-truncate error rather than a positive recovery
  boundary. Partition truncate remains under the ownerless partition-DDL
  rejection policy, and temporary-table truncate remains separate.
- Broader native redo/checkpoint reconciliation, active-reader pressure crash
  breadth, and external MariaDB/RQG stress remain open completion gates.
