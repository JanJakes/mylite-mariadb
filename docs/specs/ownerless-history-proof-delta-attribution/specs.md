# Ownerless History Proof Delta Attribution

## Problem

The varint compact sparse page-log slice reduced sparse-run metadata overhead,
but the reduced ownerless autocommit attribution sample still showed the
largest remaining payload in SYS history-proof data and user/index nonzero
bytes. Existing history-proof attribution proves the simple insert path
publishes one rollback-segment `FIL_PAGE_TYPE_SYS` proof page and one
`FIL_PAGE_UNDO_LOG` proof page per insert, but it does not prove whether those
full page images mostly contain stable bytes.

Before replacing the proof representation, MyLite needs direct evidence for
same-page history-proof deltas: how often the proof page identity repeats, how
many accepted proof pages are first samples or cache evictions, and how many
bytes actually change when the same proof identity is published again.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` arms
  `mylite_ownerless_history_proof_active` around the commit mini-transaction
  and records both the rollback-segment and undo-header page identities needed
  for the current ownerless history proof.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_history_proof_roles()` maps a committed page image to
  the active proof roles, and `ownerless_page_write_note_history_proof_page()`
  marks proof success only after accepted page-version publication.
- The same file already contains a diagnostics-only same-page byte-diff sampler
  for canonical `FIL_PAGE_TYPE_TRX_SYS` images. The measured simple insert path
  has zero canonical TRX_SYS samples, so the next sampler must target the
  active history-proof roles instead.
- `packages/libmylite/tests/embedded_performance_probe.c` and
  `packages/libmylite/tests/ownerless_cross_process_sql_test.c` mirror the
  MariaDB-side page-publish counter order; any new counters must be appended.

## Design

Append diagnostics-only page-publish counters for accepted history-proof pages:

- rollback-segment proof page samples, first samples, same-identity diff
  samples, cache evictions, total changed bytes, header/trailer changed bytes,
  and body changed bytes;
- undo-header proof page samples, first samples, same-identity diff samples,
  cache evictions, total changed bytes, header/trailer changed bytes, and body
  changed bytes.

The sampler keeps a small process-local direct-mapped cache per proof role,
keyed by `(space_id,page_no,page_size)`. A first sample or cache replacement
stores the accepted page image and increments first-sample accounting. A
same-key sample compares the accepted image against the cached prior image,
records byte-diff totals, then updates the cache. Cache evictions are counted
so the probe can distinguish "no repeats" from "the diagnostic cache is too
small".

The counters are active only under the existing ownerless page-publish stats
flag and run only after `mylite_ownerless_innodb_publish_page_version()`
accepts the page image. They do not change page-version WAL encoding,
retention, checkpointing, native storage, or SQL behavior.

## Scope And Non-Goals

In scope:

- Attribute accepted rollback-segment and undo-header history-proof page deltas.
- Preserve existing page-publish counter indexes and append new counters.
- Emit raw and per-insert probe summaries for the new counters.
- Add focused assertions that the new counters account for observed proof pages.

Out of scope:

- Adding delta-encoded page-version WAL records.
- Eliding history-proof page images.
- Changing redo/checkpoint reconciliation, purge, undo history, or recovery.
- Broad DDL/file lifecycle recovery or external MariaDB/RQG stress.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli, wire-protocol, storage-engine,
storage-format, or directory-layout behavior changes. The slice adds internal
diagnostics only.

## Database Directory And Lifecycle Impact

No durable files or shared-memory layout changes. The diff cache is
process-local, stats-gated, and reset with the existing page-publish stats.

## Native Storage Impact

No native InnoDB page format or recovery behavior changes. A later optimization
may use this evidence to justify a narrower proof or delta representation, but
that later slice must separately prove peer visibility, forced `.shm` rebuild,
native checkpoint/recovery, and WAL reclaim behavior.

## Build, Size, And Dependency Impact

No new dependency. Because the counters live in `mtr0mtr.cc`, the MariaDB
embedded archive must be rebuilt before verification. The only size impact is
small stats code plus a bounded process-local diagnostic page cache used when
page-publish stats are enabled.

## Test And Verification Plan

- Rebuild the production MariaDB embedded archive after editing InnoDB source.
- Rebuild focused embedded production targets.
- Run focused ownerless history-proof and native-support elision selectors.
- Run the ownerless primitive page-log tests as a readback/encoding guard.
- Run a reduced stats-enabled production embedded performance probe and record
  history-proof delta evidence.
- Run production build guards, CI production audit, format check, and
  whitespace check.

## Implementation Evidence

The implementation appends page-publish counters after the existing
history-proof publication and elision-blocked counters, preserving earlier
counter indexes. `mtr0mtr.cc` samples only accepted native-support page-version
publications whose page identity matches the active history-proof role. Each
role has a bounded direct-mapped process-local page cache. First samples and
cache replacements update the cached image; same-identity samples record
changed bytes split into InnoDB file header/trailer versus page body bytes.

The embedded performance probe mirrors the appended counters and emits both raw
totals and ownerless-autocommit per-insert summaries. Focused ownerless SQL
coverage asserts that proof-role samples equal the existing accepted
history-proof publication counters, that first plus diff samples equals total
samples, that evictions cannot exceed first samples, and that changed-byte
buckets add up to changed-byte totals.

The reduced 100-row production stats-enabled performance probe reported:

- rollback-segment proof samples: `1.000` per insert;
- rollback-segment first samples: `0.990` per insert;
- rollback-segment same-identity diff samples: `0.010` per insert;
- rollback-segment cache evictions: `0.950` per insert;
- rollback-segment changed bytes: `0.110` per insert, split into `0.080`
  file header/trailer bytes and `0.030` body bytes;
- undo-header proof samples: `1.000` per insert;
- undo-header first samples: `0.990` per insert;
- undo-header same-identity diff samples: `0.010` per insert;
- undo-header cache evictions: `0.950` per insert;
- undo-header changed bytes: `0.400` per insert, split into `0.080` file
  header/trailer bytes and `0.320` body bytes;
- page-log payload: `6077.410` bytes per insert, including `4152.330` SYS
  payload bytes and `1761.200` index payload bytes.

This evidence says the immediate hot path is not dominated by repeated
same-identity proof-page images. A later optimization should first address
history-proof identity churn or broader redo/checkpoint proof semantics before
adding a simple last-image delta format.

## Verification Results

Local verification on 2026-06-12 used `build/mariadb-embedded` with the
production `MinSizeRel` baseline and `build/embedded-prod` with first-party
`Release` artifacts:

- `tools/mariadb-embedded-build build` passed and rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset embedded-prod -R '^libmylite\.ownerless-primitives$'
  --output-on-failure` passed.
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-history-wal-proof` passed.
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-native-support-page-wal-elision` passed.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed and produced the
  evidence above.
- A matching reduced stats-off production probe passed. It reported ownerless
  warm open/close at `394.433 ms` versus ordinary `384.090 ms`, ownerless
  active-runtime reconnect at `1.006 ms` versus ordinary `1.948 ms`,
  ownerless direct `SELECT 1` ratio `0.9917`, and ownerless prepared
  `SELECT 1` ratio `0.9220`.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `tools/require-cmake-release-build build/embedded-prod` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- `tools/require-cmake-release-build build/ownerless-test-hooks` passed.
- `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure` passed.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-stress --output-on-failure` did not complete:
  `libmylite.ownerless-cross-process-checksum-stress` timed out after
  900 seconds with `innodb_fatal_semaphore_wait_threshold was exceeded for
  dict_sys.latch`. A direct `checksum-stress` rerun reproduced the same fatal
  wait before all children signaled ready, and the hung parent/remaining
  children were terminated and their `/tmp/mylite-ownerless-*` directory was
  removed. The new attribution sampler is gated by
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS` and is not active in this stress
  run; this remains a broader ownerless startup/open stress blocker.
- `ctest --preset ownerless-stress -E 'checksum' --output-on-failure` ran the
  first four non-checksum stress cases successfully, then
  `libmylite.ownerless-cross-process-random-transaction-stress` aborted once
  with a deterministic expected-total mismatch. A direct `random-tx-stress`
  rerun passed, and the same CTest-registered random transaction stress passed
  in isolation.
- `ctest --preset ownerless-stress -R
  'foreign-key|child-failure|active-reader|expanding|blob'
  --output-on-failure` passed, covering FK graph, child-failure cleanup,
  active-reader pressure, expanding-page pressure, BLOB pressure, compressed
  BLOB pressure, and the active-reader/BLOB trace tools.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Focused SQL tests prove history-proof delta samples are counted for the
  accepted proof pages.
- First-sample plus same-identity diff samples equal each role's proof samples,
  and changed-byte buckets add up to changed-byte totals.
- The performance probe reports whether the remaining proof payload has
  repeated same-identity pages with small byte deltas.
- No page-version WAL format, native storage, or compatibility behavior changes
  are introduced.

## Risks And Unresolved Questions

- This is attribution, not a runtime speedup. It must feed a later proof or
  delta-encoding slice before performance improves.
- A direct-mapped diagnostic cache can evict identities under broader
  workloads; eviction counters make that visible.
- If the hot path keeps allocating new undo-space proof page identities, a
  same-page delta format may not help that path even if individual pages have
  stable bytes after reuse.
