# Perf Baseline Profile Iterations

## Goal

Let the local performance baseline run explicitly longer profiling samples while
keeping ordinary benchmark invocations bounded. Recent prepared row-update and
storage mutation samples were short enough that setup and cold cache frames
could dominate the profiler output, which makes later hot-path choices weaker.

## Non-Goals

- Do not change benchmark phases, measured operations, or default iteration
  counts.
- Do not make long-running performance tests part of default CTest coverage.
- Do not change SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c::parse_config()` accepts two positional
  numeric arguments for rows and iterations and currently caps both at
  `1000000`.
- `tools/mylite_perf_baseline.c::print_usage()` documents the focused phases
  and opt-in `--max-us` threshold model.
- `docs/ROADMAP.md` records current local prepared-update evidence from
  `1000000`-iteration samples and identifies table-open / DML prepare /
  `JOIN::prepare()` overhead as the next wall.

## Compatibility Impact

No MySQL/MariaDB compatibility impact. This is a local benchmark harness
change only. `docs/COMPATIBILITY.md` does not need an update.

## Design

Add an explicit `--profile-iterations=<n>` option to
`tools/mylite_perf_baseline.c`. The ordinary positional `iterations` argument
keeps the existing `1000000` cap, preserving the bounded default shape for
quick local checks and regression gates.

`--profile-iterations` accepts up to `100000000` iterations and conflicts with
the positional iterations argument. This keeps accidental long runs out of
normal command lines while making profiler-oriented samples straightforward:

```sh
tools/mylite-perf-baseline --phase=prepared-row-only-update-components \
  --profile-iterations=10000000 10000
```

## File Lifecycle

No `.mylite` file lifecycle change. Longer local benchmark runs still use the
same temporary benchmark root and the existing `MYLITE_PERF_KEEP_ROOT=1`
debugging escape hatch.

## Embedded Lifecycle And API

No public `libmylite` API change. Longer benchmark runs exercise existing
embedded open/execute/close behavior through the same harness setup.

## Build, Size, And Dependencies

No new dependency and no embedded runtime profile change. The code change is
limited to the benchmark tool parser and help text.

## Test Plan

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline -j1`
- Verify a normal focused phase still runs with ordinary positional iterations.
- Verify an ordinary positional iteration value above `1000000` is rejected.
- Verify `--profile-iterations=1000001` runs successfully.
- Verify duplicate iteration sources are rejected.

Current local verification:

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline -j1`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-scalar-selects 1 1000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-scalar-selects --profile-iterations=1000001 1`
- Rejected `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-scalar-selects 1 1000001`
- Rejected `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-scalar-selects --profile-iterations=100000001 1`
- Rejected `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-scalar-selects --profile-iterations=1000001 1 1000`

## Acceptance Criteria

- Default benchmark behavior and positional caps remain unchanged.
- Long profiling iteration counts require an explicit option.
- The option is documented in tool usage and development docs.
- Parser errors reject accidental duplicate iteration inputs.

## Risks And Open Questions

- Long local runs remain machine-dependent and should not become hard project
  performance claims without recorded hardware and command context.
- This only improves measurement quality; it does not itself reduce prepared
  update execution cost.
