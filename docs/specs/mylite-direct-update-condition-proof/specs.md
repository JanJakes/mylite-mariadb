# MyLite Direct Update Condition Proof

## Problem

Accepted MyLite direct updates already read one candidate row through the exact
unique-key predicate proved during planning. For the common predicate shape:

```sql
UPDATE t SET c = c + 1 WHERE id = ?
```

`ha_mylite::direct_update_rows()` still evaluates the original `WHERE`
condition after the exact key read. That final condition evaluation is required
for predicates with additional conjuncts, but it is redundant when the entire
condition is exactly the same non-null single-part unique-key equality used to
read the row.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/opt_range.cc::find_simple_unique_key_equal_item()` and
  `mariadb/storage/mylite/ha_mylite.cc::mylite_find_direct_update_equal_item()`
  both recurse through `AND` conditions and accept a direct-update key proof
  when any conjunct is a supported exact unique-key equality.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` pushes
  the SQL-layer exact-key proof to the MyLite handler through
  `INFO_KIND_MYLITE_UPDATE_EXACT_KEY` when the handler direct-update path is
  otherwise eligible.
- `ha_mylite::direct_update_rows()` reads the candidate with
  `read_exact_unique_index_row_into()` and then calls
  `direct_update_condition->val_bool()` before applying the update.
- Existing prepared-update coverage includes pure `id = ?`, reversed
  `? = id`, bound `NULL`, and an additional `AND value < 100` predicate that
  must still evaluate the non-key conjunct before reporting affected rows.

## Design

1. Extend the existing SQL-layer exact-key marker with a boolean that records
   whether the matched equality item is the top-level condition item.
2. Carry that proof through `INFO_KIND_MYLITE_UPDATE_EXACT_KEY` to avoid a
   second handler-side condition walk on the hot prepared path.
3. Add a handler-local boolean proof for the accepted direct-update condition
   and reset it whenever direct-update condition state is reset.
4. For `cond_push()` fallback paths that do not have the SQL marker, set the
   proof only when the top-level condition itself is the supported key-field
   equality and the matched value item is the accepted direct-update value
   item.
5. Keep `AND` predicates conservative: they can still use the direct-update
   key proof, but they must keep evaluating the full condition after the row is
   read.
6. In `direct_update_rows()`, skip `val_bool()` only for the proven pure
   condition and keep the existing `THD` error check before mutating the row.

The key read remains authoritative only for row discovery. MariaDB expression
evaluation remains authoritative for any condition shape broader than the exact
key predicate.

## Affected MariaDB Subsystems

- SQL exact-key marker code in `mariadb/sql/opt_range.{h,cc}` and
  `mariadb/sql/sql_update.cc`.
- MyLite storage handler direct-update code in
  `mariadb/storage/mylite/ha_mylite.{h,cc}`.

No parser, optimizer, catalog, file-format, public API, or wire-protocol
behavior changes.

## Compatibility Impact

Affected-row behavior is unchanged:

- no-match and bound-`NULL` exact-key values still report zero affected rows,
- unchanged-row comparisons still use MariaDB's row comparison path,
- additional predicates still run through `Item::val_bool()`.

The skipped condition is limited to one supported non-null raw integer unique
key equality that has already been converted through the handler's
`build_direct_update_key()` path.

## DDL Metadata Routing Impact

No DDL metadata routing changes.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, locking, recovery, or handle-lifecycle changes.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No engine routing-policy change. The optimization applies only after the
ordinary MyLite direct-update gates accept a routed MyLite table.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes.

## Binary-Size Impact

The slice adds one handler boolean and a small condition-shape helper. It adds
no dependency and should have neutral archive-size impact, measured through the
storage-smoke embedded archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Reuse existing prepared primary-key update coverage for pure equality,
  reversed equality, bound `NULL`, extra `AND` predicates, unchanged rows,
  duplicate-key errors, and secondary-index correctness.
- Run prepared-update component and full prepared-update performance baselines.
- Run a sampled prepared-update pass and confirm the pure direct-update path no
  longer spends visible samples evaluating `direct_update_condition->val_bool()`
  for the benchmark's `id = ?` predicate.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates keep current affected-row behavior for pure equality,
  reversed equality, bound `NULL`, and extra predicates.
- The handler skips final condition evaluation only when the direct-update
  condition is exactly the key equality that produced the exact row read.
- No new sidecars, public API changes, file-format changes, or dependencies are
  introduced.

## Risks And Unresolved Questions

- The proof must stay conservative around `AND` predicates, even when one
  conjunct is the exact key equality.
- The optimization depends on the current direct-update raw-key subset. Broader
  future key types should keep the proof disabled until conversion and
  comparison equivalence has been reviewed for those types.
