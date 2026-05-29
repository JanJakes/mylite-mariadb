# Dirty-Page Buffer Merge Guard Free-Slot Detail

## Problem

After full and near-full future-current index leaves direct-write during
dirty-page buffer merge, the current prepared-insert profile still reports
`51,341` dirty `index-leaf` pressure admissions. Existing guard leaf-shape
counters classify all remaining `future-current-header-partial-leaf` rows as
`16+` free slots, but that bucket is too broad to justify another direct-write
threshold. The next slice needs narrower evidence without changing production
merge behavior.

## Source Findings

- MariaDB authority: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MyLite dirty-page buffer merge policy lives in
  `packages/mylite-storage/src/storage.c`:
  `merge_dirty_page_buffer()`,
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()`,
  `direct_write_dirty_page_buffer_merge_entry()`, and
  `record_dirty_page_buffer_merge_direct_write_guard_outcome()`.
- Pressure admission evidence is recorded in
  `record_dirty_page_buffer_pressure_incoming_page()` and exposed through
  `tools/mylite_perf_baseline.c` prepared-insert component output.
- Existing coarse free-slot classification uses `0`, `1`, `2-3`, `4-7`,
  `8-15`, and `16+`. The `16+` bucket now contains the whole remaining dirty
  leaf pressure class after the near-full direct-write policy.

## Design

Add test-hook-only free-slot detail bands for valid index leaves:

- `0`
- `1`
- `2-3`
- `4-7`
- `8-15`
- `16-31`
- `32-63`
- `64-127`
- `128+`

Keep the existing coarse bands for continuity. Record the new detail bands for
pressure incoming index leaves and for merge direct-write guard outcomes, then
print both tables in the prepared-insert benchmark. This is an evidence slice:
it does not change `merge_dirty_page_buffer()` direct-write eligibility,
dirty-buffer admission, rollback, append-buffer handling, or file format.

## Compatibility And Lifecycle Impact

There is no SQL, C API, storage-engine routing, DDL metadata, wire-protocol,
durable file-format, or embedded lifecycle behavior change. Durable state
remains in the primary `.mylite` file plus the existing MyLite-owned journal
lifecycle. The added counters are compiled only under
`MYLITE_STORAGE_TEST_HOOKS` and are not public compatibility surface.

## Tests And Verification

- Add focused storage self-test coverage for every free-slot detail band on
  pressure incoming leaves and merge direct-write guard leaf counters.
- Extend the prepared-insert benchmark output with pressure incoming and guard
  outcome detail tables.
- Run the standard storage and storage-smoke verification:
  `git diff --check`, `git clang-format --diff`, dev storage build and CTest,
  static MariaDB storage-smoke archive build, storage-smoke build and CTest,
  and the prepared-insert component benchmark.

## Acceptance Criteria

- Detail counters classify valid index leaves across `0` through `128+` free
  slots and count invalid metadata separately.
- Prepared-insert output reports the detail shape of remaining
  `future-current-header-partial-leaf` rows and pressure incoming leaves.
- Existing coarse counters and direct-write behavior remain unchanged.
- Docs record the new evidence before any wider direct-write policy is chosen.

## Implementation Evidence

The storage-smoke prepared-insert component profile on this VPS reports
`72.818 us/op` for the prepared insert step. The remaining `51,341` pressure
incoming dirty `index-leaf` rows and the matching
`future-current-header-partial-leaf` guard rows split as:

| Leaf free slots | Pages |
| --- | ---: |
| `16-31` | `15,491` |
| `32-63` | `19,321` |
| `64-127` | `14,523` |
| `128+` | `2,006` |

The same profile still reports `3,793`
`future-current-header-direct-write` full leaves and `30,797`
`future-current-header-near-full-direct-write` leaves, with `34,590` dirty
leaf merge direct writes total. Production merge behavior is unchanged by this
slice.

## Risks And Unresolved Questions

- The detail counters are evidence, not proof that a wider partial-leaf
  direct-write threshold is profitable or rollback-safe.
- If most rows cluster in `16-31`, a follow-up implementation slice still needs
  to validate performance against the prior broad direct-write regression.
