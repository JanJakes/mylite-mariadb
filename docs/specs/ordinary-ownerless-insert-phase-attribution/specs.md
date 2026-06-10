# Ordinary Versus Ownerless Insert Phase Attribution

## Problem

The ownerless branch already prints detailed ownerless autocommit attribution,
but the ordinary comparator mainly exposes throughput. That makes the remaining
performance gap easy to over-attribute to ownerless page publication even when
part of the cost is shared MariaDB/InnoDB insert or commit work.

This slice adds a stats-gated ordinary baseline for the same deep InnoDB commit
and row-insert counters used by the ownerless attribution probe. The goal is to
show ownerless-minus-ordinary phase deltas from production builds without
slowing or perturbing the default stats-off throughput run.

## Source Findings

- `packages/libmylite/tests/embedded_performance_probe.c` already runs
  ordinary and ownerless direct selects, prepared selects, transactional
  inserts, and autocommit inserts in one probe.
- The probe already emits detailed ownerless page-publish, page-log,
  page-write, handler, and deep InnoDB counters when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- The InnoDB deep counter gate is global rather than ownerless-only despite the
  exported `mylite_ownerless_innodb_deep_*` names, so it can measure ordinary
  inserts without changing SQL or storage behavior.
- The default CI embedded performance run remains stats-off. CI runs a separate
  reduced stats-enabled attribution probe under `php-embedded-prod`, with
  `Release` MyLite and `MinSizeRel` MariaDB embedded build guards.

## Design

When `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`:

- enable only the deep InnoDB counter gate before the ordinary insert loops,
- reset the existing counter set at the same point as the measured insert loop,
- emit raw `mylite_perf_ordinary_insert_(txn|autocommit)_innodb_deep_*`
  counters,
- snapshot ordinary autocommit deep counters after the ordinary autocommit
  loop,
- snapshot ownerless autocommit deep counters after the ownerless autocommit
  loop, and
- emit compact `mylite_perf_summary_*` ordinary baselines plus
  `mylite_perf_summary_ownerless_minus_ordinary_*` per-insert deltas for the
  commit, write-history, history-list, commit-in-memory, ownerless-visibility,
  row-insert, and clustered optimistic B-tree phases.

Keep existing detailed metric keys unchanged. Do not enable page-publish,
page-write, page-log, handler, or database ownerless counters during the
ordinary baseline; those counters describe ownerless integration paths and
would add noise to the ordinary engine comparator.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, storage-engine, metadata, locking, or
directory-lifecycle behavior changes. The slice changes only diagnostics
printed by the embedded performance probe when an existing opt-in stats flag is
set.

## Build And Performance Impact

The default stats-off performance probe is unchanged. The stats-enabled
attribution probe performs extra counter reads and `printf()` calls after the
measured loops, and enables deep InnoDB counters for ordinary insert loops only
inside the already diagnostic attribution mode.

The CI timing claim remains production-build based: normal throughput still
comes from the stats-off production probe, and attribution comes from the
separate reduced production probe with build-type guards.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` under `php-embedded-prod`.
- Run a reduced stats-enabled production probe and confirm the new ordinary
  raw deep counters and ownerless-minus-ordinary summary deltas are present.
- Run a reduced stats-off production probe to confirm default throughput output
  still runs without the new attribution-only counter gates.
- Run `tools/check-ci-production-builds`.
- Run production format and whitespace checks.
