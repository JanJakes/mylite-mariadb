# Ownerless Dictionary Ready Fast Path

## Problem

Current production probes show that ownerless active-runtime reconnect is not
the main PHPUnit or embedded-runtime cost. Repeated ownerless statements still
pay statement-boundary coordination work, including an ownerless dictionary
ready wait, even when the handle has already observed the current stable
dictionary generation and no dictionary DDL is active.

`mylite_ownerless_dictionary_state_wait_ready()` is correct but relatively
heavy for the already-ready case because it builds a timeout deadline and can
enter the wait-owner path. For hot plain reads and small writes, the common
case only needs to prove that the shared dictionary generation is still the
same stable idle generation the handle already observed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_dictionary_state.cc`
  stores the ownerless dictionary generation at offset 0. Stable generations
  are even; active DDL generations are odd and carry an active owner id,
  owner generation, and owner pid.
- `mylite_ownerless_dictionary_state_wait_ready()` loops until the generation
  is even and no active owner is present, returning the stable generation.
- `mylite_ownerless_dictionary_state_read_snapshot()` reads the same
  generation and active-owner fields without constructing a wait deadline.
- `packages/libmylite/src/database.cc`
  `refresh_ownerless_dictionary_before_statement()` already compares the ready
  generation with `mylite_db::ownerless_observed_dictionary_generation` and
  returns without flushing caches when they match.
- The same function still performs the full wait and refresh path when the
  generation changes or DDL is active.

## Design

Add a private `database.cc` helper that checks the cheap ready case before the
full wait path:

- the handle must have initialized an observed dictionary generation;
- the dictionary-state snapshot must read successfully;
- no dictionary DDL owner may be active;
- the snapshot generation must be even; and
- the snapshot generation must equal the handle's observed generation.

When all predicates hold, `refresh_ownerless_dictionary_before_statement()`
returns immediately. Any active DDL, changed generation, unreadable mapping, or
uninitialized handle observation falls through to the existing
`mylite_ownerless_dictionary_state_wait_ready()` and cache-refresh logic.

The fast path is intentionally not a DDL lock. It only preserves the existing
ready-generation semantics while avoiding extra wait setup in the already-idle
unchanged case.

## Compatibility Impact

No SQL behavior, public C API, PHP API, wire-protocol behavior, storage format,
or unsupported-surface policy changes. Statements still observe dictionary
generation changes through the existing full refresh path.

## Directory And Lifecycle Impact

No new files, durable records, shared-memory layout, or lifecycle state are
introduced. The helper reads existing `mylite-concurrency.shm` dictionary
generation fields.

## Native Storage Impact

No native InnoDB, MyISAM, Aria, or MariaDB dictionary file format changes.
Native dictionary cache flushing still happens after observed dictionary
generation changes.

## Build And Performance Impact

The change is first-party MyLite code only and does not require rebuilding the
MariaDB embedded archive. It removes repeated wait-deadline setup and
active-owner wait plumbing from statements whose handle already observed the
same stable dictionary generation.

The expected effect is modest but broad: direct/prepared ownerless statements
avoid one piece of repeated statement-boundary overhead while larger remaining
costs stay in page-version publication, page-log append, native commit, and
history-proof volume.

## Test And Verification Plan

- Build production embedded MyLite targets for the performance probe and
  ownerless SQL harness.
- Run stats-off direct/prepared `SELECT 1` and insert probes before and after
  the change to confirm the run remains comparable and no regression is
  obvious.
- Run focused ownerless selectors that depend on read visibility and dictionary
  refresh: `prepared-committed-read`, `local-write-first-read`, and a DDL
  refresh selector.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Stable already-observed dictionary generations skip the heavier ready-wait
  path.
- Active or changed dictionary generations still use the existing wait and
  refresh path.
- Focused ownerless read and DDL refresh selectors pass.
- Production stats-off probes remain valid and do not show an obvious
  ownerless read/write regression.

## Risks And Follow-Up

- The fast path removes overhead rather than changing the dominant ownerless
  write proof cost. It is not a substitute for the remaining native
  history-proof, redo/checkpoint, DDL recovery, active-reader policy, or
  external stress work.
- A future broader read fast path must still respect baseline read pins and
  monotonic page-version visibility; this slice does not alter those rules.
