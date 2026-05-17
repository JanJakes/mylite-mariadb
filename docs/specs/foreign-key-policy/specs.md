# Foreign-Key Policy

## Goal

Reject foreign-key DDL explicitly through `libmylite` until MyLite storage has
catalog metadata, enforcement, locking, recovery, and transaction semantics for
referential constraints.

The later [Foreign-key foundation](../foreign-key-foundation/specs.md) design
defines the boundary for implementing those prerequisites without weakening
this rejection policy prematurely.

This protects routed `ENGINE=InnoDB` tables from implying InnoDB-compatible
foreign-key behavior while the MyLite handler is still non-transactional and
does not expose foreign-key metadata hooks.

## Non-Goals

- Implement foreign-key metadata storage in the `.mylite` catalog.
- Enforce parent/child row checks on `INSERT`, `UPDATE`, `DELETE`, `DROP`, or
  `ALTER`.
- Implement cascading actions, `foreign_key_checks` import semantics, or
  deferred checks.
- Decide CHECK constraints, generated columns, or unsupported physical index
  classes in this slice.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy`: `column_def` accepts column-level `references`,
  `key_def` accepts `FOREIGN KEY`, and `ALTER TABLE` accepts `ADD` and
  `DROP FOREIGN KEY` forms.
- `mariadb/sql/sql_lex.cc`: `LEX::add_table_foreign_key()` and
  `LEX::add_column_foreign_key()` append `Foreign_key` entries and mark
  `ALTER_ADD_FOREIGN_KEY`.
- `mariadb/sql/sql_parse.cc`: `check_fk_parent_table_access()` walks parsed
  foreign keys before `CREATE TABLE` / `ALTER TABLE` execution.
- `mariadb/sql/sql_table.cc`: copy `ALTER` has dedicated foreign-key checks,
  foreign-key metadata calls, and parent/child lock handling.
- `mariadb/sql/handler.h`: `HTON_SUPPORTS_FOREIGN_KEYS` marks engines with
  foreign-key support; handlers expose `get_foreign_key_list()`,
  `get_parent_foreign_key_list()`, and `referenced_by_foreign_key()`.
- `mariadb/storage/mylite/ha_mylite.h`: MyLite advertises
  `HA_NO_TRANSACTIONS` and does not advertise foreign-key support.
- Official MariaDB docs describe foreign keys as referential constraints for
  InnoDB tables and note that foreign keys require a supporting storage engine:
  <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
  and
  <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>.

## Compatibility Impact

Applications may request `ENGINE=InnoDB`, but MyLite currently routes that
metadata to the MyLite handler. Accepting foreign-key DDL before enforcement
exists would be a compatibility bug: applications could believe referential
integrity is protected when it is not.

This slice changes the compatibility matrix from "planned" to "partial" for
foreign keys: MyLite now covers explicit rejection for `CREATE TABLE` /
`ALTER TABLE` foreign-key DDL, while real InnoDB-compatible enforcement remains
planned.

## Design

Add a `libmylite` SQL policy check for foreign-key DDL:

- `CREATE TABLE ... FOREIGN KEY ...` rejects.
- `CREATE TABLE ... REFERENCES ...` rejects column-level references.
- `ALTER TABLE ... ADD ... FOREIGN KEY ...` rejects.
- `ALTER TABLE ... DROP FOREIGN KEY ...` rejects as an unsupported metadata
  surface until MyLite has persisted FK metadata.

The detector scans SQL tokens while skipping comments, quoted identifiers, and
string literals so ordinary values such as `COMMENT 'REFERENCES'` do not trip
the policy. It deliberately stays at the public API boundary rather than
patching broad SQL-layer internals, because the current storage smoke uses
`libmylite` as the product contract and raw MariaDB access remains an optional
future adapter.

`SET foreign_key_checks` is not rejected in this slice. Since foreign-key DDL is
rejected, toggling MariaDB's session variable cannot make MyLite accept or
enforce FK metadata. Import-oriented handling for that variable should be
specified separately when dump compatibility is tested.

## File Lifecycle

Rejected statements must not create `.frm`, InnoDB, MyISAM, Aria, or MyLite
catalog sidecars. Storage-engine smoke tests must confirm the primary `.mylite`
catalog is unchanged after rejected FK DDL and the runtime directory is clean
after close.

## Embedded Lifecycle And API

`mylite_exec()` and `mylite_prepare()` return `MYLITE_ERROR`, leave MariaDB
errno at zero, set SQLSTATE `HY000`, and expose the stable message
`unsupported foreign-key SQL surface`.

The policy applies before direct execution or prepared-statement compilation,
so repeated open/close behavior and statement ownership do not change.

## Build, Size, And Dependencies

No new dependencies and no meaningful size impact. The implementation is
first-party `libmylite` policy code and tests.

## Test Plan

- Direct embedded API: representative `CREATE TABLE` table-level FK,
  column-level `REFERENCES`, and `ALTER TABLE ... ADD FOREIGN KEY` reject with
  the stable MyLite diagnostic.
- Prepared API: preparing FK DDL rejects before MariaDB prepare.
- Storage-engine smoke: routed `ENGINE=InnoDB` parent/child tables still support
  ordinary DDL/DML, FK DDL rejects before catalog publication, disabling
  `foreign_key_checks` does not allow FK DDL, and sidecar cleanup remains clean.
- Compatibility harness: add a `foreign-key` group over the storage-smoke build.
- Verification: targeted tests, full configured CTest presets, formatting,
  tidy, harness report, and `git diff --check`.

## Acceptance Criteria

- Foreign-key DDL cannot be accepted through `mylite_exec()` or
  `mylite_prepare()`.
- Rejected FK statements use stable MyLite diagnostics rather than incidental
  MariaDB handler errors.
- Routed `ENGINE=InnoDB` tables remain usable for supported non-FK DDL/DML.
- Compatibility docs and roadmap state explicit FK rejection and keep real
  enforcement planned.
- The compatibility harness can run the foreign-key evidence by name.

## Risks And Open Questions

- `SET foreign_key_checks` is common in SQL dumps. Leaving it accepted is
  intentional for this slice, but dump-import compatibility still needs a
  dedicated policy and tests.
- Some MariaDB shorthand `REFERENCES` forms have changed semantics across
  versions. MyLite rejects them conservatively until FK support exists.
- The optional raw MariaDB adapter will need equivalent policy or clearly
  documented lower-level semantics before it is exposed.
