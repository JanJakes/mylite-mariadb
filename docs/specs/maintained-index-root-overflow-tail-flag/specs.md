# Maintained Index Root Overflow Tail Flag

## Problem

Root-backed readers must scan the append tail after a maintained root has
overflowed into append-only index-entry pages. The previous overflow-tail fix
restored correctness by starting the tail overlay at `root_page + 1` for every
maintained root. That is conservative, but it makes ordinary maintained roots
with no overflow history lose the static no-tail fast path once unrelated pages
exist after the root.

The read path needs a durable per-root signal that distinguishes "this root has
fallback append-tail entries" from "this root is the complete index snapshot".

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB handler exact index reads still enter MyLite through the same
  `handler::index_read*()` contract; this slice changes only first-party
  storage root selection.
- `packages/mylite-storage/src/storage_format.h` already reserves a maintained
  index-root flags field and defines
  `MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_SINGLE_PAGE`.
- `decode_maintained_index_root_page()` currently rejects any flags other than
  `SINGLE_PAGE`.
- `plan_maintained_index_root_inserts()` detects a full maintained root and
  leaves the append-only index-entry write enabled for the overflowing row.
- `read_index_leaf_run_root()` can select the append-tail start for maintained
  roots after decoding the root page flags.

## Scope

- Add a maintained-root overflow-tail flag in the existing flags field.
- Set the flag when an insert overflows a full maintained root into append-only
  index-entry pages.
- Preserve the flag when a later root-resident delete reduces the root entry
  count below capacity, so existing overflow-tail entries remain visible.
- Let maintained roots without overflow history keep `tail_page_id =
  header->page_count`.

## Non-Goals

- No root split, merge, or multi-page maintained index.
- No catalog format change.
- No new public API.
- No attempt to compact or absorb overflow-tail entries back into the root.

## Design

Use `MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_HAS_OVERFLOW_TAIL` in the existing
maintained-root flags field. Decoding accepts only `SINGLE_PAGE` plus this new
known flag.

Insert planning should record full maintained roots as overflow roots while
leaving the matching `index_entry_changed` bit enabled. Those root pages are
included in the preplanned dirty-page set, then rewritten through the pager to
set the overflow flag before the append-only index-entry page is published. If
the bounded preplanned dirty-page set cannot protect that flag write, the insert
must fail rather than publish a hidden append-tail entry.

Deletes preserve an existing overflow flag. A full root without that explicit
flag remains a complete single-page root; root fullness alone is not treated as
overflow history.

Maintained-root insert, update, and delete paths must keep their dirty root-page
rewrites protected. If the active journal shape cannot protect those pages, the
mutation fails instead of publishing append-only fallback state that would be
invisible once reads rely only on the explicit overflow flag.

Reads use the append-tail overlay only when the overflow-tail flag is set.

## Compatibility Impact

SQL-visible behavior remains unchanged. This is a performance and correctness
guard for the internal storage root path.

## Single-File And Lifecycle Impact

The existing root page flags field changes within the primary `.mylite` file.
Dirty root-page writes use the same pager and rollback-journal protection as
maintained-root entry rewrites. No new sidecar files are introduced.

## Public API And File-Format Impact

No public API change. The file format uses one additional known bit in an
existing maintained-root flags field, without changing page layout or format
version.

## Storage-Routing Impact

Durable MyLite-routed tables keep correct root-plus-tail visibility with less
unnecessary tail scanning. Volatile engines are unaffected.

## Binary-Size, License, And Dependency Impact

No new dependency or imported code. Binary impact is limited to a small
first-party storage change.

## Test Plan

- Extend maintained-root overflow-tail storage coverage to assert the new flag
  is set after a full root overflows.
- Delete a root-resident row after overflow, then verify the overflow-tail row
  remains visible before and after any refill.
- Keep a refilled full root without an overflow flag from re-enabling tail scans
  during later in-place updates.
- Keep overflow-row update/delete coverage and maintained-root rollback/recovery
  tests passing.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- New maintained roots can report no append tail until they overflow.
- Maintained roots with overflow history keep append-tail visibility until a
  later refill proves all live entries fit in the single-page root again.
- Full maintained roots without overflow history do not scan stale append-tail
  pages.
- Unknown maintained-root flags remain corruption.
- Existing maintained-root insert, update, delete, rollback, and recovery
  coverage remains green.

## Initial Implementation

- Added `MYLITE_STORAGE_FORMAT_INDEX_ROOT_FLAG_HAS_OVERFLOW_TAIL` to the
  maintained-root flags field and kept decode strict to known flag bits.
- Insert planning records full maintained roots as overflow roots, journals
  them with the existing preplanned dirty-page set, and sets the overflow flag
  before publishing the append-only fallback index-entry page.
- Maintained-root deletes preserve an existing overflow flag.
- Maintained-root insert, update, and delete paths reject unprotected dirty-root
  fallback plans.
- Root reads scan the append tail only when the overflow flag is set.
- The overflow-tail storage regression now asserts the flag after overflow,
  deletes a root-resident row to reduce the root below capacity, and verifies
  the overflow row stays visible through exact and full index reads.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/src/storage_format.h packages/mylite-storage/tests/storage_test.c`

## Risks And Open Questions

- The flag is still an interim marker, not a substitute for root splits. Full
  SQLite-like index performance still needs navigable multi-page maintained
  indexes.
- Very large single-row insert plans that exceed the bounded dirty-page
  preplan now fail rather than publishing hidden fallback entries. A later
  split/merge design should remove this append-tail overflow mode.
