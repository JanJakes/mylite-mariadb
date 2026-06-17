# Ownerless History Proof Fallback Guard

## Problem

The current ownerless write-performance bottleneck still includes one
rollback-segment `FIL_PAGE_TYPE_SYS` proof page and one `FIL_PAGE_UNDO_LOG`
proof page for simple autocommit inserts. Those pages are tempting
optimization targets, but the current proof contract in
`trx_t::write_serialisation_history()` is explicit: if the ownerless
page-version WAL does not publish both expected history pages, MyLite must use
the conservative native exact history flush.

This slice adds a focused guard for that contract before any later
history-proof shrink/elision work.

## Source Findings

- Base: MariaDB 11.8.6 import
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` arms
  `mylite_ownerless_history_proof_active`, commits the history mini-
  transaction, and accepts the ownerless WAL proof only when the commit LSN is
  nonzero, no page publish failed, and both the rollback-segment and undo
  history-proof pages were published.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_note_history_proof_page()` marks the expected proof
  pages only after accepted page-version publication.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_page_publish_hook()` is the first-party hook that can
  report a page-version publish failure back to the InnoDB MTR path.

## Design

Add an unsafe-hook-only environment switch,
`MYLITE_OWNERLESS_TEST_FAIL_NATIVE_SUPPORT_PAGE_PUBLISH=1`, that makes the
page-publish hook fail native-support page images while
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS` and the InnoDB test-fault switch
are active. Normal production builds ignore the switch.

Add a focused selector,
`history-proof-publish-failure-fallback`, that:

- creates an ownerless InnoDB table;
- enables page-publish, commit-visibility, and deep InnoDB counters;
- fails native-support page publication for one visible-fast insert;
- verifies page publish failure was observed;
- verifies no history-proof page publication was accepted;
- verifies native ownerless history flush counters are positive;
- verifies COMMIT used the conservative flush path because of publish failure;
- verifies same-handle, ownerless reopen, and forced-`.shm` native reopen
  visibility.

## Compatibility Impact

No SQL, public C API, PHP, mysqli, wire protocol, durable file, or production
runtime behavior changes. The new failure switch is compiled only into unsafe
ownerless hook builds.

## Performance Impact

No production speedup is claimed. The slice protects the next performance work
by proving that the current full history-proof page publication cannot be
silently removed without either preserving the WAL proof or taking the native
flush fallback.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run `history-proof-publish-failure-fallback` directly.
- Run adjacent ownerless history/native-support selectors in production builds.
- Run the hook negative-proof CTest subset, production build guards, format
  check, and whitespace check.

## Acceptance Criteria

- The unsafe-hook selector observes page-publish failure and positive native
  history flush counters.
- The committed row remains visible through ownerless and native reopen.
- Normal production builds keep ignoring the failure switch.
- Docs identify this as a guardrail, not a completion or speedup claim.
