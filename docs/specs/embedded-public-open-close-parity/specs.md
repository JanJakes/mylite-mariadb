# Embedded Public Open/Close Parity

## Problem

The ownerless branch has deep internal performance probes, but `origin/main`
predates those probes and the production CMake presets. That made branch/main
startup comparisons hard to reproduce when investigating slow PHPUnit runs:
the branch could attribute startup and shutdown phases, while main could only
be compared through a public API workload.

## Source Findings

- Branch under test: `ownerless-concurrency`
  (`793a085c2c2d25b74610ae1df1f965569bfe4013`).
- Main comparison ref: `origin/main`
  (`4760d5128096e4560bc62cc19f7066bc15ff07d8`).
- The common public API surface across both refs includes `mylite_open()`,
  `mylite_close()`, `mylite_exec()`, `mylite_errmsg()`,
  `MYLITE_OPEN_READWRITE`, `MYLITE_OPEN_CREATE`, and the leading
  `mylite_open_config` fields through `temp_directory`.
- Main does not have `php-embedded-prod`,
  `mylite_embedded_performance_probe`, or ownerless fields/flags, so the
  branch's internal probe cannot be used directly as a parity comparator.
- Both refs link `libmylite.a` against the MariaDB embedded archive and the
  same system dependencies, so a standalone public C benchmark can compare the
  process-style open/close cost without depending on branch-only internals.

## Design

Add `tools/mylite-public-open-close-bench.c`, built as
`mylite_public_open_close_bench` when `MYLITE_WITH_MARIADB_EMBEDDED=ON`.

The benchmark accepts:

```text
mylite_public_open_close_bench <database-path> <runtime-root> <iterations>
```

It performs one prepare cycle that opens a directory database, creates an
`app.public_open_close_probe` InnoDB table, inserts one row, and closes the
database. It then measures repeated warm public `mylite_open()` plus
`mylite_close()` cycles against the same directory. It prints parseable keys
for prepare milliseconds, iteration count, total milliseconds, and average
milliseconds.

The benchmark deliberately uses only public API fields and functions present
on both refs. It does not use ownerless flags, internal MariaDB counters, or
branch-only diagnostic symbols.

## Compatibility Impact

No SQL, C API, PHP API, storage format, wire-protocol, or directory-layout
behavior changes. The new target is a measurement tool only.

## Build And Size Impact

The benchmark is a small executable under `tools/`. It links against
`MyLite::mylite` only when embedded MariaDB support is enabled. It does not
change the library, PHP extensions, installed public headers, or runtime
dependencies.

## Test And Verification Plan

- Build the target with the production embedded preset.
- Compile the same source manually against `origin/main`, because main lacks
  the target and production preset.
- Run alternating clean branch/main samples with the same iteration count.
- Run the branch internal embedded performance probe to reconcile public
  benchmark cost with internal startup and shutdown attribution.
- Run format and production-build audits before committing.

## Verification Results

The branch target built with:

```text
cmake --preset php-embedded-prod
cmake --build --preset php-embedded-prod --target mylite_public_open_close_bench
```

For `origin/main`, the same source was compiled manually against
`build/php-embedded-prod-manual/packages/libmylite/libmylite.a` and
`build/mariadb-embedded/libmysqld/libmariadbd.a` after configuring a manual
Release build equivalent to the branch production preset.

After filesystem caches were warm, two alternating clean 20-iteration samples
reported:

| Ref | Prepare ms | Warm open/close avg ms |
| --- | ---: | ---: |
| ownerless-concurrency `793a085c` | `470.548` | `144.442` |
| origin/main `4760d512` | `687.845` | `346.062` |
| ownerless-concurrency `793a085c` | `421.940` | `141.430` |
| origin/main `4760d512` | `582.270` | `347.400` |

The ownerless branch is therefore not slower than main for this public
process-style open/close path. In the warmed samples it is about `0.41x` of
main's warm open/close cost.

A current-head refresh on 2026-06-22, after the WordPress PHPUnit CI
performance slices, compared ownerless-concurrency
`8d6f13986a69cd8d1126f4db2d6ef20b8d175c5a` with the same main ref. Main was
rebuilt in `/tmp/mylite-main-perf-worktree` with a manual Release embedded
configuration, then the same public benchmark source was compiled against
main's `libmylite.a` and `libmariadbd.a`. Two alternating clean
20-iteration samples reported:

| Ref | Prepare ms | Warm open/close avg ms |
| --- | ---: | ---: |
| ownerless-concurrency `8d6f13986` | `565.594` | `131.458` |
| origin/main `4760d512` | `1153.496` | `380.739` |
| ownerless-concurrency `8d6f13986` | `496.718` | `125.558` |
| origin/main `4760d512` | `829.580` | `370.065` |

The current branch remains ahead of main on this public process-style
open/close path; the warmed samples are about `0.33-0.36x` of main's warm
open/close cost.

A matching branch internal probe with 20 warm open/close iterations reported:

- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=138.025`;
- `open_total_ms_avg=112.727`;
- `open_start_runtime_ms_avg=111.086`;
- `close_total_ms_avg=25.295`;
- `startup_storage_engine_init_innodb_ms_avg=71.868`;
- `shutdown_innodb_shutdown_total_ms_avg=23.071`;
- `shutdown_innodb_logs_empty_sleep_ms_avg=1.077`;
- `ordinary_active_runtime_reconnect_ms_avg=0.891`.

The matching 2026-06-22 current-head internal probe reported:

- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=128.592`;
- `open_total_ms_avg=102.490`;
- `open_start_runtime_ms_avg=100.643`;
- `close_total_ms_avg=26.098`;
- `startup_storage_engine_init_innodb_ms_avg=53.269`;
- `startup_innodb_srv_start_total_ms_avg=53.162`;
- `startup_innodb_srv_start_recovery_bootstrap_ms_avg=30.385`;
- `startup_innodb_srv_start_log_rebuild_ms_avg=0.561`;
- `startup_innodb_srv_start_system_tables_ms_avg=16.141`;
- `ordinary_active_runtime_reconnect_ms_avg=0.896`.

The high-cost path is full embedded MariaDB startup/shutdown per process.
Once the runtime is active inside a process, reconnect is sub-millisecond in
this sample. The remaining startup target is native MariaDB/InnoDB
initialization, especially InnoDB `srv_start()` and storage-engine plugin
initialization, rather than ownerless coordination setup.

## Risks And Follow-Up

These numbers are local timing evidence, not a CI threshold. CI still records
branch-side production probe output, while branch/main parity requires a
second checkout or worktree. The benchmark is intentionally small so it can be
used for future parity checks without importing branch-only diagnostic APIs
into main comparisons.
