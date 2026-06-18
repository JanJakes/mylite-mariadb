# Ownerless Streamed Insert Row-Count Batch

## Problem Statement

The stepwise visible-fast append-batch slices raised the proven
`INSERT ... VALUES` row-list cap from four through sixty-four rows. Follow-up
production probes on 2026-06-18 showed that 128-row and 256-row bulk inserts
already used one page-log append session and deferred latest-checkpoint
coalescing even though the explicit cap was still sixty-four rows. That means
large row lists were being admitted through the fixed SQL policy token window
rather than through a reliable full-statement row count.

This is both a correctness and performance problem. The current behavior keeps
bulk timing fast for large row lists, but it hides the real boundary and can
undercount statements whose row-list tokens exceed `k_sql_policy_token_count`.
The next slice must preserve the measured 256-row fast path while making the
eligibility proof explicit and bounded.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc`
  `collect_sql_policy_tokens()` stores only `k_sql_policy_token_count` tokens.
  That bounded snapshot is appropriate for broad policy checks, but it is not
  enough to count large `INSERT ... VALUES` row lists.
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` previously counted rows from
  that bounded token array. A large row list could therefore look smaller than
  the configured append-batch cap.
- `packages/libmylite/src/database.cc`
  `next_sql_token()` already provides the streaming tokenizer used by policy
  collection. Reusing it over the original SQL text preserves existing token
  handling for quotes, identifiers, punctuation, spacing, and comments without
  increasing the global policy snapshot size.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` still publishes ownerless page
  versions at mini-transaction boundaries. Batching only changes the MyLite
  page-log append-session lifetime around those existing publications.

## Design

Replace the token-buffer row count with a streaming row-list counter over the
original SQL text. The counter keeps the existing pure-row-list rules:

- the first token must be `INSERT`;
- the first top-level `VALUE` or `VALUES` token begins the row-list parser;
- each row constructor must be parenthesized;
- nested parentheses inside values are allowed;
- unsupported tails such as `ON DUPLICATE KEY UPDATE`, `RETURNING`, and
  `INSERT ... SELECT` stay ineligible.

Raise `k_ownerless_append_batch_fast_path_max_insert_values_rows` from
sixty-four to 256 after the 128-row and 256-row production probes showed the
same bounded statement-level append-session pattern. Row lists above 256 stay
visible-fast eligible when they are pure row lists, but they do not defer
page-log append sessions or coalesce latest-only checkpoint updates.

## Scope And Non-Goals

In scope:

- direct and prepared ownerless `INSERT ... VALUES` statements with one through
  256 streaming-proven row constructors;
- explicit focused coverage for the 256-row positive boundary;
- explicit focused coverage for the 257-row conservative boundary.

Out of scope:

- unbounded row-list append batching;
- broad DML/DDL batching;
- group commit across statements or processes;
- changing page-version WAL records, checkpoint records, native InnoDB page
  images, native history-proof publication, or redo/checkpoint recovery.

## Compatibility Impact

No SQL result, public C API, PHP/mysqli behavior, metadata format, native
storage format, wire protocol, or directory layout changes. Eligible commits
become visible at the same logical boundary; the slice only makes the existing
large-row-list performance behavior explicit and capped.

Statements that are not pure row-list inserts remain conservative. A pure
257-row statement still uses visible-fast commit publication, but it does not
hold one append session across the full SQL statement.

## Database Directory And Native Storage Impact

No durable files or directory paths change. The ownerless page-version WAL,
checkpoint file, shared-memory segments, and native InnoDB files keep the same
layout and durability rules. Native history-proof rollback-segment and undo
pages are still published.

## Build And Performance Impact

The code change stays in first-party MyLite statement policy and test code. It
does not touch upstream-derived MariaDB source or add dependencies.

Expected 256-row signal:

- visible-fast commit remains at one per SQL statement;
- page-log append-session begin/end calls are one per SQL statement;
- deferred latest-checkpoint coalescing remains active;
- 257-row statements show more than one append-session begin/end call and zero
  deferred latest-checkpoint coalescing.

## Test And Verification Plan

- Extend `single-owner-multi-row-insert-visible-fast-path` to assert a 256-row
  visible-fast insert publishes page versions, keeps native history proof, uses
  exactly one append session, and coalesces latest-checkpoint updates.
- Add a 257-row case that proves the cap is enforced without disabling
  visible-fast commit publication.
- Build the production embedded ownerless SQL harness and performance probe.
- Run the focused ownerless selector directly and through CTest.
- Run adjacent production ownerless selectors for primitives, native-support
  page WAL elision, visible-fast multi-row inserts, and FK fast-path cache.
- Run 128-row and 256-row production performance probes after the streaming
  counter change.
- Run hook/stress coverage for the focused selector, production-build guards,
  format checks, and `git diff --check`.

## Acceptance Criteria

- 256-row pure ownerless `INSERT ... VALUES` statements use one page-log append
  session while preserving visible-fast commit publication.
- 257-row pure ownerless `INSERT ... VALUES` statements remain outside the
  append-batch and deferred-checkpoint-coalescing path.
- Unsupported row-list tails stay ineligible.
- Production probe evidence shows the 128-row and 256-row fast paths remain
  active after removing token-window undercounting.

## Implementation Evidence

The implementation changes
`ownerless_insert_values_statement_row_count()` to stream over the original SQL
text with `next_sql_token()` instead of counting rows from the bounded
`SqlPolicyTokens` array. `ownerless_statement_fast_path_policy()` now receives
that SQL text for both direct and prepared execution paths, and
`k_ownerless_append_batch_fast_path_max_insert_values_rows` moves from `64` to
`256`.

`test_ownerless_single_owner_multi_row_insert_visible_fast_path()` now adds:

- a 256-row pure `INSERT ... VALUES` case that verifies visible-fast commit,
  zero conservative flush, zero ownerless history flush, native history WAL
  proof publication, one page-log append session, deferred latest-checkpoint
  coalescing, and durable row visibility;
- a 257-row pure `INSERT ... VALUES` case that verifies visible-fast commit
  still succeeds while append batching is not used and deferred latest-
  checkpoint coalescing remains zero.

Local production evidence on 2026-06-18:

- Pre-slice 128-row probe:
  `MYLITE_PERF_INSERT_ITERATIONS=1280`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=128`, and ownerless page-publish
  stats enabled reported `10` append-session begin/end calls for `10`
  statements, `1317` session append calls, visible-fast commit at `1.000` per
  statement, conservative flush at `0.000`, deferred latest-checkpoint
  coalescing at `256.000` per statement, and ownerless 128-row bulk throughput
  at `14006.72` rows/s.
- Post-slice 128-row probe with the same shape kept `10` append-session
  begin/end calls, `1317` session append calls, visible-fast commit at `1.000`
  per statement, conservative flush at `0.000`, deferred latest-checkpoint
  coalescing at `256.000` per statement, and ownerless 128-row bulk throughput
  at `12776.56` rows/s. The ordinary comparison moved from `121411.98` to
  `59697.86` rows/s in the local sample, so the ratio moved from `0.1154` to
  `0.2140` while the ownerless fast-path counters stayed stable.
- Pre-slice 256-row probe:
  `MYLITE_PERF_INSERT_ITERATIONS=2560`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=256`, and ownerless page-publish
  stats enabled reported `10` append-session begin/end calls for `10`
  statements, `2609` session append calls, visible-fast commit at `1.000` per
  statement, conservative flush at `0.000`, deferred latest-checkpoint
  coalescing at `512.000` per statement, and ownerless 256-row bulk throughput
  at `14526.11` rows/s.
- Post-slice 256-row probe with the same shape kept `10` append-session
  begin/end calls, `2609` session append calls, visible-fast commit at `1.000`
  per statement, conservative flush at `0.000`, deferred latest-checkpoint
  coalescing at `512.000` per statement, and ownerless 256-row bulk throughput
  at `12178.41` rows/s. The ordinary comparison moved from `145951.38` to
  `100657.46` rows/s in the local sample, and the ownerless ratio moved from
  `0.0995` to `0.1210`.

## Risks And Follow-Up

The 256-row cap remains a lock-hold-time tradeoff. It is intentionally bounded
because an earlier unbounded row-list attempt regressed bulk timing. Larger row
lists, broader DML batching, native history-proof replacement, group commit,
and native redo/checkpoint reconciliation remain separate work.
