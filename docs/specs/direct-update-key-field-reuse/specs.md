# Direct Update Key Field Reuse

## Goal

Reduce exact-key prepared `UPDATE` direct-path overhead by avoiding repeated
`store_key_item` key-field clone allocation while building the direct lookup
key.

## Non-Goals

- New direct-update predicate shapes.
- New key types beyond the current one-part non-null integer raw-key subset.
- Bypassing MariaDB item-to-field conversion, null handling, or truncation
  behavior for lookup-key materialization.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_select.h::store_key` constructs a cloned key field with
  `Field::new_key_field()` in its constructor.
- `store_key_item::copy_inner()` resets the cloned key field, saves the value
  item into it, restores the table write-set map, and reports null/fatal
  conversion state.
- MyLite direct update already admits only one-part non-null unique integer
  keys where `mylite_key_uses_raw_exact_filter()` is true and the key-part
  length matches the field key length. Charset narrowing is therefore outside
  this accepted direct-update key subset.
- Local prepared-update sampling after direct inner row writes still shows
  `Field::new_key_field()` / `alloc_root()` in the direct-update hot path.

## Design

Add handler-owned direct-update key materialization state:

1. A reusable key buffer.
2. A cloned key field allocated on the opened table's `mem_root` for the
   currently accepted direct-update key number.
3. A key null byte used by MariaDB field save/null checks.

Build each direct-update lookup key by resetting the cached cloned key field,
saving the value item into it under the same relaxed field-copy and
`CHECK_FIELD_IGNORE` behavior as the existing `store_key_item` path, and
checking `field->is_null()` / `item->null_value` before accepting the key.

If the accepted direct-update key number changes, allocate a fresh cached key
field for that key. The table memroot owns those clones for the opened table
lifetime.

## Compatibility Impact

No SQL surface changes. Accepted prepared point updates should keep the same
lookup key bytes, no-match behavior for null parameters, diagnostics, affected
row counts, and rollback behavior as the current direct-update path.

Unsupported shapes keep falling back before this materialization path.

## File Lifecycle

No storage format, durable file, or sidecar changes. This is handler scratch
state only.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build the storage-smoke embedded storage-engine test and performance tool.
- Run the storage-smoke capability, embedded comparison, and embedded storage
  tests.
- Run prepared-update performance baselines and sample the accepted direct path
  to confirm `Field::new_key_field()` drops out of MyLite's direct key builder.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused storage-smoke tests pass.
- Prepared primary-key updates with integer parameters keep current behavior.
- The sampled prepared-update hot path no longer shows repeated
  `Field::new_key_field()` key-field clone allocation under MyLite's direct
  key builder.
- Prepared-update timing does not materially regress.

## Risks And Open Questions

- This must not broaden direct-update support to string, nullable, multi-part,
  virtual, or BLOB/TEXT key shapes.
- The cached cloned field is table-lifetime scratch and must not outlive the
  opened handler/table state.
- `SQL_SELECT::check_quick()` still performs its own generic quick-plan key
  build before the handler direct hook. Removing that belongs to a separate
  SQL-layer slice.
