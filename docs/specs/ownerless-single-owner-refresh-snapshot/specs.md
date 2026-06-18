# Ownerless Single-Owner Refresh Snapshot

## Problem

The ownerless refresh attribution slice showed hot InnoDB point selects using
the local-native-current-read path, with almost all refresh time spent in the
shared redo/process/transaction snapshot. In the single-owner case, the refresh
snapshot still scans process and transaction registries to prove facts that the
process registry active count and generation already prove: no other ownerless
process is live in the current epoch.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `refresh_ownerless_external_pages_before_statement()` snapshots ownerless
  redo state, process generation, transaction active count, explicit
  transaction state, single-owner epoch state, and page-pin registry pointers
  before choosing local-native-current-read or page-version visibility.
- `ownerless_runtime_in_single_owner_epoch_locked()` already defines a
  single-owner epoch as process-registry active count `1` and process-registry
  generation equal to the current process slot generation.
- `ownerless_process_registry_has_other_live_explicit_transactions()` excludes
  the current owner id. In a single-owner epoch, no other owner id is active, so
  the result is necessarily false.
- `ownerless_trx_registry_has_other_active_transactions()` returns whether the
  transaction active count exceeds active transactions owned by the current
  owner. In a single-owner epoch, no other owner id is active, so the result is
  necessarily false; the separate aggregate `active_trx_count` remains available
  for native-write-state decisions.
- `ownerless_runtime_has_no_live_explicit_transactions()` includes the current
  owner. In a single-owner epoch this can be computed by reading the current
  process slot's explicit transaction count instead of scanning all process
  slots.

## Design

Inside the refresh shared-snapshot block, read process-registry active count,
generation, and the current owner slot's explicit transaction count while the
runtime mutex is already held. When active count is `1` and the registry
generation equals the current process slot generation:

- set `single_owner_epoch` directly;
- set `no_other_live_explicit_transactions` to true;
- set `no_live_explicit_transactions` from the current owner slot explicit
  transaction count;
- set `no_other_active_transactions` to true.

When the process registry does not prove a single-owner epoch, keep the existing
conservative scans and helper calls. Do not change redo-state reads,
page-version pin policy, clean-page refresh, native read-view handling, or peer
join/leave generation checks.

## Scope And Non-Goals

In scope:

- first-party MyLite refresh snapshot logic;
- production probe evidence using the existing refresh attribution counters;
- documentation of the proof and remaining risks.

Out of scope:

- changing page-version visibility, native read-view publication, or page-pin
  lifetime;
- skipping refresh work when peers are live or when process generation does not
  prove the single-owner epoch;
- changing SQL behavior, public C API behavior, durable directory layout, or
  native storage format.

## Compatibility And Storage Impact

No MySQL/MariaDB SQL behavior, public MyLite API behavior, ownerless directory
layout, or native storage format changes. The optimization only avoids registry
scans whose answers are already proven by the process registry in a
single-owner epoch.

## Test Plan

- Build `mylite_embedded_performance_probe` in `php-embedded-prod`.
- Run a reduced stats-enabled production probe and compare point-select refresh
  shared-snapshot timing.
- Build `mylite_ownerless_cross_process_sql_test`.
- Run focused ownerless selectors for tableless read fast path and
  visible-fast insert safety.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Single-owner refresh snapshots skip process/trx registry scans whose result is
  already implied by process active count and generation.
- Non-single-owner refresh snapshots keep the existing conservative helper
  calls.
- Focused ownerless SQL selectors and production probe build pass.

## Implementation Evidence

A reduced local `php-embedded-prod` stats-enabled attribution run on 2026-06-18
used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=100 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

After the single-owner inference, ownerless direct point-select refresh dropped
to `0.002 ms/select` total with `0.001 ms/select` in the shared snapshot, and
ownerless prepared point-select refresh dropped to `0.002 ms/select` total with
`0.001 ms/select` in the shared snapshot. Both point-select forms still reported
`1.000` local-native-current-read decisions per select, zero page-version reads,
zero handle-pin registrations, zero clean-page refreshes, and one current
read-view close attempt per select.

A reduced stats-off 1000-select sample used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=1000 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported ownerless direct point-select throughput at `2909.58 ops/s` versus
ordinary direct point-select throughput at `3223.01 ops/s`, ratio `0.9028`.
Ownerless prepared point-select throughput was `3087.72 ops/s` versus ordinary
prepared point-select throughput at `4006.58 ops/s`, ratio `0.7707`, leaving
prepared native execute overhead as the next read-path target.

## Risks

This optimization relies on the process registry active count and generation
being the authority for the single-owner epoch. If a future ownerless mode adds
same-process owner ids or changes process-slot ownership semantics, the proof
must be revisited before extending the fast path.
