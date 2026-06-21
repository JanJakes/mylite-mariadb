# Ownerless MTR Native Support Skip Cache

## Problem

The production page-write probe shows that remaining 16384-row ownerless bulk
insert statements publish no page-version WAL records, but still perform about
`32782` mini-transaction commit-log calls and about `16448` native-support
transaction publish skips per statement.

Those MTRs have already classified the modified page as a transaction-held
native-support page when acquiring page-write ownership. The no-dirty
commit-log publish path rechecks the transaction native-support set before it
can skip publication.

## Design

Remember one positive native-support publish-skip page in `mtr_t`. The slot is
set only when `ownerless_page_write_enter()` proves that the page is already
held as native-support state by the transaction, or when it first records the
page into the transaction native-support set. The slot is reset with each MTR.

During no-dirty commit-log publishing, the MTR-local proof is checked before
the transaction-set lookup. The fast path still refuses to run while detailed
page-publish or page-write perf stats are enabled, still requires a valid
commit LSN and read-write non-dictionary transaction, and still rechecks
history-proof roles so rollback-segment and undo proof pages are not elided
accidentally.

The existing exact transaction native-support skip path remains the fallback.

## Compatibility Impact

No SQL behavior, C API, PHP API, redo format, page-version WAL format,
checkpoint format, shared-memory layout, or directory layout changes.

## Native Storage Impact

Native InnoDB page contents, redo LSNs, undo records, ownerless page-version
records, and page-write lock ownership are unchanged. The slice only avoids a
repeated transaction-set lookup for a page already proven native-support safe in
the same MTR.

## Test And Verification Plan

- Build the embedded MariaDB archive and focused production MyLite targets.
- Run focused ownerless native-support and visible-fast SQL selectors.
- Run stats-enabled and stats-off 16384-row production probes and compare
  remaining bulk throughput and page-write attribution against the pre-slice
  samples.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- MTR-local native-support skip proofs cannot outlive `mtr_t::start()` or
  `mtr_t::release_resources()`.
- Existing native-support WAL-elision and visible-fast correctness selectors
  pass.
- Production stats-off bulk throughput does not regress; preferably the
  remaining-statement ownerless rate moves closer to ordinary.
- Stats-enabled attribution remains on the existing exact path so CI/debug
  timings stay comparable.

## Verification

- `tools/mariadb-embedded-build build`: passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`:
  passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)$|^libmylite\.embedded-ownerless-innodb-lock-hooks$'
  --output-on-failure`: passed.
- `ctest --preset php-embedded-prod -R
  '^tools\.ownerless-(transaction-stress-trace|active-reader-pressure-trace)$'
  --output-on-failure`: passed.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

Production stats-off probe, 32768 inserts with 16384-row bulk statements:

- Pre-slice sample:
  ownerless bulk `88041.77 rows/s`; ownerless remaining bulk
  `55999.65 rows/s`.
- Post-slice sample A:
  ownerless bulk `101426.76 rows/s`; ownerless remaining bulk
  `66441.47 rows/s`.
- Post-slice sample B:
  ownerless bulk `98002.56 rows/s`; ownerless remaining bulk
  `64382.21 rows/s`.

The host was noisy during the final probes, with other Codex/Chrome/daemon
processes active. Non-bulk select and single-row insert microbenchmarks varied
widely across adjacent runs, so the acceptance evidence is limited to the
targeted bulk-write path and the focused correctness selectors.

Production page-write-stats probe keeps the exact stats path active:

- Pre-slice sample:
  remaining native-support publish skips `16448/statement`;
  no-dirty page-publish `13.368 ms/statement`.
- Post-slice sample:
  remaining native-support publish skips `16448/statement`;
  no-dirty page-publish `13.870 ms/statement`.
