# Foreign-Key Same-Row SET NULL Update

## Goal

Cover the bounded self-referencing `ON UPDATE SET NULL` case where the row being
updated is also a child row that still references its own old parent key. For
example, updating `(id, parent_id) = (1, 1)` to `id = 10` should publish the row
as `(10, NULL)` instead of rejecting the update because `parent_id = 1` no
longer has a parent.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/row/row0ins.cc:row_ins_foreign_check_on_constraint()`
  handles `UPDATE_SET_NULL` by building an update vector that sets the child
  fields participating in the FK to SQL `NULL`.
- The same function routes action updates through
  `row_update_cascade_for_mysql()` after finding matching child records in the
  child index. MyLite does not have this recursive action graph yet, so the
  same-row case needs a local pre-publication rewrite instead of a second
  handler update on the same row.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_row()` currently
  prepares the updated row's index entries before parent FK actions run. That
  ordering is correct for other-row actions, but a same-row `SET NULL` rewrite
  must happen before duplicate-key checks, child FK checks, row payload
  serialization, and index-entry publication for the row being updated.
- `mariadb/sql/field.h:Field::set_null()` supports setting a nullable field in
  a specific record buffer by passing a row offset, which lets MyLite update
  the `new_data` record buffer before index preparation.
- MyLite parent FK checks inspect the still-live old index entries before the
  current row update is published. The parent check therefore must ignore the
  current self-referencing row only when the new child key no longer references
  the old parent key; external child rows and unchanged self-restrict cases
  must still block the update.

Official MariaDB documentation describes `SET NULL` as a storage-engine FK
action and requires nullable child columns:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Scope

Implement this bounded SQL subset:

- durable MyLite-routed base table;
- self-referencing `ON UPDATE SET NULL`;
- old row's referenced parent key is changing;
- old row's child FK key equals the old referenced parent key;
- new row's child FK key is either still the old parent key or already `NULL`;
- nullable child FK columns over the current simple SET NULL table shapes.

The existing other-row self and non-self `SET NULL` action paths remain
responsible for rows other than the row being updated.

## Non-Goals

- Overriding explicit same-row child-key rewrites to a different non-NULL
  parent in the same SQL update. Those continue through ordinary immediate
  child/parent FK checks.
- Recursive action chains, cascades, `SET DEFAULT`, and full InnoDB action
  graph compatibility.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file action support.
- Broader multi-row matrices that combine same-row rewrites with additional
  action chains.

## Design

Before `ha_mylite::update_row()` prepares index entries for `new_data`, MyLite
should list parent FK metadata for the updated table. For each self-referencing
`ON UPDATE SET NULL` metadata entry:

1. resolve parent and child keys on the current `TABLE`;
2. return early if the old parent key is `NULL` or the parent key is unchanged;
3. return early if the old child key is `NULL`;
4. encode the old parent key into child-key format and compare it to the old
   child key;
5. if the old row was not a same-row child of the old parent key, do nothing;
6. inspect the new child key and do nothing when it was explicitly rewritten to
   a different non-NULL key;
7. otherwise set the child FK columns in the `new_data` record buffer to SQL
   `NULL`.

The existing update path then recomputes index entries and row payload from the
rewritten `new_data`, runs duplicate-key and child-FK checks over that buffer,
applies other parent actions, and performs the normal parent checks. During
those parent checks, self-referencing FK lookup skips the current row id only
when the updated child key is `NULL` or rewritten to a different key. That
prevents the old same-row child index entry from blocking its own update while
preserving external-child rejection.

## Compatibility Impact

MyLite can claim the common same-row self `ON UPDATE SET NULL` case over simple
nullable child columns. Full same-row action compatibility remains partial
because explicit same-row child-key rewrites to a different non-NULL parent are
treated as ordinary SQL updates rather than action overrides.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. The same-row rewrite is
part of the parent row update that is already protected by the current
statement checkpoint.

## Public API, Build, Size, License

No public API, build profile, dependency, license, or intentional size-profile
change is introduced.

## Test And Verification Plan

- Extend storage-engine smoke coverage so a self-referencing row `(1, 1)` with
  `ON UPDATE SET NULL` becomes `(10, NULL)` when `id` changes.
- Verify the rewritten row's index entries match the `NULL` child key and the
  old parent key can no longer be referenced.
- Verify the behavior survives close/reopen.
- Keep coverage where explicit child-key rewrites to a valid parent continue to
  work through ordinary immediate checks.
- Keep the existing parent-check ordering coverage passing so external children
  still reject parent-key updates after the current row is ignored.
- Run the storage-smoke archive rebuild, focused storage-engine smoke binary,
  `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Same-row old-key self `ON UPDATE SET NULL` updates no longer fail with a stale
  child reference.
- Index entries and row payloads are based on the rewritten row.
- Existing other-row self and non-self `SET NULL` tests still pass.
- Docs and compatibility matrices describe the bounded same-row support without
  claiming full FK action graph compatibility.

## Risks And Open Questions

- MariaDB/InnoDB's exact behavior for updates that explicitly rewrite the same
  child FK columns in the same statement needs broader compatibility evidence
  before MyLite should claim full parity.
- Future recursive action work may replace this local pre-publication rewrite
  with a general action graph executor.
