# Coalesced Statement Buffer Lookups

## Problem

The prepared-update profile still shows MyLite-owned statement-chain lookups in
the hot path after page accessor inlining. Two hot paths first call
`active_statement_for_file()` and then immediately call
`append_page_buffer_statement_for_file()`, which walks the same active statement
chain again.

This matters because prepared row-DML repeatedly rewrites buffered row and
index pages inside an active statement or transaction. The chain is short, but
the repeated lookup is still visible in the sampled profile.

## Source Findings

- MariaDB base line: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite-owned source: `packages/mylite-storage/src/storage.c`.
- `active_statement_for_file()` returns the nearest matching statement owned by
  the active context.
- `append_page_buffer_statement_for_file()` returns the outermost matching
  statement owned by the active context. It intentionally keeps scanning after a
  match.
- `buffer_append_pages_at_raw()` and `rewrite_active_update_pages()` call both
  helpers back-to-back for the same `FILE *`.
- The post-accessor prepared-update sample still lists
  `active_statement_for_file()` and `rewrite_active_update_pages()` as hot
  MyLite-owned symbols.

## Design

Add a private helper that scans the active statement chain once and returns both
roles:

- `active_statement`: the first matching statement for the current owner.
- `append_buffer_statement`: the last matching statement for the current owner.

Use the helper only in call sites that currently need both values for the same
file. Preserve the existing read-snapshot exclusion and `NULL` handling.

Do not change the single-value helpers. Other call sites only need one role, and
keeping those helpers intact keeps the diff small.

## Compatibility Impact

No SQL-visible behavior change.

## File And API Impact

No public API, file-format, or companion-file change.

## Storage Routing Impact

No routing change.

## Binary-Size Impact

Negligible. This adds one private helper and removes duplicate loop work from two
hot call sites.

## Test And Verification Plan

- Build first-party storage smoke targets.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run prepared-update benchmark repeats.
- Sample a prepared-update run and check whether active statement lookup samples
  drop in or around `buffer_append_pages_at_raw()` and
  `rewrite_active_update_pages()`.

## Acceptance Criteria

- Storage-smoke coverage remains green.
- Active statement and append-buffer statement roles retain their existing
  nearest-vs-outermost semantics.
- Prepared-update timing does not regress materially.

## Risks

- Collapsing the two lookups incorrectly could make nested checkpoints use the
  wrong append buffer. The helper must keep first-match and last-match behavior
  distinct.

## Verification

Environment: AppleClang 21.0.0.21000101 on macOS, storage-smoke preset, MariaDB
embedded archive built with `-DPLUGIN_MYLITE_SE=STATIC`.

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_embedded_storage_engine_test`: passed.
- `git diff --check`: passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: passed, 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000`: prepared primary-key update repeats were 4.206, 4.152, and
  4.155 us/op.
- `sample` over a prepared-update run no longer showed
  `append_page_buffer_statement_for_file()` in the searched markers. The new
  coalesced helper appeared at 5 top-stack samples, and remaining
  `active_statement_for_file()` samples came from other call sites.
