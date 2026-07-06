# Ownerless Process Identity Liveness

## Problem

Ownerless cleanup previously relied on process IDs for process-registry
liveness. A PID-only check is not sufficient for durable directory coordination:
the kernel can reuse a PID after the original owner exits, and a reused PID must
not keep stale ownerless transaction, lock, read-view, or page-version state
alive.

Zombie processes are another Linux-specific edge case. `kill(pid, 0)` can still
succeed after the owner process has exited but before its parent reaps it; the
owner can no longer release MyLite shared state, so cleanup must treat that slot
as dead.

## Source Findings

- `packages/libmylite/src/ownerless_process_registry.*` owns the fixed process
  registry used by product ownerless opens and primitive tests.
- `packages/libmylite/src/database.cc` reads process slots to decide live-peer
  presence, explicit-transaction presence, stale-slot cleanup, and no-live
  native shutdown/recovery behavior.
- `packages/libmylite/src/ownerless_probe.*` is the ownerless platform gate used
  before product ownerless opens.
- Linux `/proc/<pid>/stat` exposes the process start-time clock-tick field, and
  `/proc/sys/kernel/random/boot_id` distinguishes process identities across
  boot sessions.

## Design

Process-registry slots now store a full process identity:

- PID,
- `/proc/<pid>/stat` start time,
- hash of `/proc/sys/kernel/random/boot_id`.

Liveness compares the stored identity against the current kernel identity for
the PID. A matching PID with a different start time or boot ID is dead owner
state, not a live peer. If `/proc` identity reads are unavailable but
`kill(pid, 0)` proves a same-PID process exists or is permission-protected, the
check remains conservative and treats it as live.

Linux zombie state is explicitly dead even when `kill(pid, 0)` succeeds.

The process-registry shared-memory segment version is bumped so stale PID-only
slots are not interpreted as identity-aware slots. Known v3 PID-only process
segments with active slots use a legacy PID-only live check before rebuild:
live legacy PIDs make the opener return `MYLITE_BUSY`, and only dead legacy
PIDs allow the volatile segment rebuild. The ownerless directory platform proof
also records `process_identity=1`; cached proofs without that field no longer
satisfy the ownerless open gate.

## Compatibility Impact

No SQL or public MyLite C API behavior changes. The internal ownerless primitive
ABI changes from PID-only callbacks to identity callbacks, and existing product
coordination files with the older process-registry segment version are treated as
incompatible volatile state and rebuilt from durable ownerless metadata only
after active legacy PID-only slots are proven dead.

Ownerless mode now requires process-identity support on the database-directory
host. Unsupported platforms fail at the ownerless platform gate rather than
claiming cross-process ownerless correctness.

## Test Plan

- Primitive coverage allocates a process slot with one identity and verifies a
  same-PID/different-start-time identity is cleaned as stale.
- Primitive coverage verifies real exited Linux child slots are reclaimable
  while the child remains unreaped.
- Platform probe coverage asserts both platform and directory probes publish
  `process_identity=1`, and the hook build can force `process-identity` probe
  failure.
- Product ownerless directory-lifecycle coverage verifies the process segment
  version and the nonzero start-time/boot-id fields in active slots.
- Product ownerless directory-lifecycle coverage seeds a v3 PID-only active
  slot with a live child PID, verifies ownerless open returns `MYLITE_BUSY`,
  then lets the child exit and verifies the opener rebuilds the segment to the
  current identity-aware layout.

## Acceptance Criteria

- A same-PID process with a different start-time/boot-id identity does not keep
  stale ownerless process-registry slots live.
- Exited zombie owners are reclaimable before parent `waitpid()`.
- Ownerless opens reject directories when process-identity support is not proven.
- Existing older volatile process-registry layouts rebuild only after active
  legacy PID-only slots are proven dead; live legacy PIDs fail closed with
  `MYLITE_BUSY`.

## Follow-Up

The dictionary-generation primitive still records only the active DDL owner's
PID in its compact 64-byte segment. Product DDL recovery is still keyed by owner
slot/generation and durable recovery markers, but PID reuse can conservatively
extend dictionary wait time until timeout. A later dictionary-state format slice
should store the same PID/start-time/boot-id identity and bump the dictionary
segment version.

## Verification Results

- `cmake --build --preset embedded-dev --target mylite_ownerless_primitives_test
  mylite_embedded_open_close_test mylite_ownerless_cross_process_sql_test`
  passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/embedded-dev/packages/libmylite/mylite_embedded_open_close_test
  ownerless-directory` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  platform-probe-failure` passed.
