# Ownerless FK Default Mixed Alter Live Recovery

## Problem

Ownerless dictionary recovery already accepts standalone
`ALTER TABLE ... ALTER COLUMN ... SET DEFAULT` and mixed foreign-key ALTER
lists that combine FK add/drop with either table `COMMENT` or `ADD COLUMN`.
The remaining mixed FK/non-FK ALTER-list gap still rejected a metadata-only
column-default change in the same statement as FK add/drop. A crash after
MariaDB applies such a native ALTER but before ownerless dictionary finish could
therefore leave a live peer unable to classify the completed DDL as recoverable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB accepts comma-separated `ALTER TABLE` specifications including
  `DROP FOREIGN KEY`, `ALTER [COLUMN] <name> SET DEFAULT <expr>`, and
  `ADD CONSTRAINT ... FOREIGN KEY ...` in one statement.
- `packages/libmylite/src/database.cc` classifies standalone column-default
  changes through `ownerless_alter_column_set_default_recovery_statement()`.
- The mixed FK recovery parser,
  `ownerless_alter_table_mixed_foreign_key_recovery_statement()`, only accepted
  FK add/drop, `ADD COLUMN`, and table `COMMENT`. `ADD COLUMN` correctly selects
  file-operation recovery; table `COMMENT` remains metadata-only.

## Design

Add a clause consumer for metadata-only `ALTER [COLUMN] <identifier> SET
DEFAULT <expr>` inside mixed foreign-key ALTER lists. The consumer requires one
default-expression token and then consumes tokens until the next comma or
semicolon, matching the existing conservative token-list parsing style for
other ALTER clauses.

Keep recovery classification unchanged:

- mixed FK plus `ADD COLUMN` continues to select add-column/file-operation
  recovery;
- mixed FK plus table `COMMENT` or column-default changes remains metadata-only
  FK recovery with a clear native file-operation marker;
- generated-column FK tables remain excluded by the existing child/referenced
  table generated-column guards.

## Compatibility Impact

No public API change. The SQL behavior follows MariaDB for a bounded ALTER
shape that is already native-supported: dropping one FK, changing a column
default, and adding another FK in one `ALTER TABLE`.

## Directory And Native Storage Impact

No new durable files or formats are added. The default metadata change is
dictionary metadata, not an InnoDB tablespace file-operation boundary. Existing
ownerless WAL/checkpoint and shared-memory state are reused.

## Test Plan

- Add a hook-build crash selector:
  `dictionary-foreign-key-mixed-default-alter-crash`.
- The test creates a child table with an existing `note INT NOT NULL DEFAULT 5`,
  drops FK A, changes `note` to default `7`, and adds FK C while a live peer is
  held open.
- Kill the writer at the deterministic `dictionary-before-finish` fault.
- Verify the native file-operation marker remains clear, the live peer recovery
  sees default `7`, FK A is absent, FK B and FK C enforce, inserts omitting
  `note` use default `7`, and ownerless/native reopen plus forced `.shm`
  rebuild preserve the state.
- Run the focused selector, adjacent mixed FK CTest subset, production build
  guards, format check, and whitespace check.

## Acceptance Criteria

- The mixed default ALTER list is accepted by ownerless dictionary recovery.
- Live-peer recovery does not retain a native file-operation marker for this
  metadata-only shape.
- Recovered default metadata and FK enforcement survive ownerless reopen,
  ordinary native reopen, and forced shared-memory rebuild.

## Risks And Follow-Up

This remains a bounded mixed ALTER-list case. Broader FK plus non-FK ALTER
lists, expression-default breadth, generated-column FK variants, rebuild
variants, and full external randomized DDL stress remain planned separately.
