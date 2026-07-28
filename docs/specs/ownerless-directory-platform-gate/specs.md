# Ownerless Directory Platform Gate

> Historical slice: this format-1 Linux gate was completed and later
> superseded by
> [Ownerless Cross-Platform Filesystems](../ownerless-cross-platform-filesystems/specs.md),
> which adds APFS/NTFS backends, format-2 platform/filesystem/volume proofs,
> pre-creation admission, and explicit unsupported-platform/filesystem errors.

## Problem

Ownerless coordination depends on filesystem semantics, not just compiled
platform support. The existing internal platform probe exercised POSIX
`MAP_SHARED`, byte-range locks, lock release on process exit, grow/remap,
wait/wake behavior, and process-identity liveness under `/tmp`, but ownerless
opens did not prove that the actual database directory's backing filesystem had
those semantics before starting the ownerless runtime.

That left two gaps:

- `mylite_capabilities()` could report ownerless modes as build-available even
  when a specific database directory was on an unproven filesystem.
- A future unsupported local mount or network filesystem could reach ownerless
  startup before failing with a less precise error.

## Source Findings

- `packages/libmylite/src/ownerless_probe.cc` had
  `mylite_ownerless_probe_platform()` using `/tmp/mylite-ownerless-probe.*`.
- `packages/libmylite/src/database.cc` validated flags and prepared the
  database directory before taking `mylite-runtime-startup.lock`, which is the
  correct point to reject a directory before ownerless hooks or MariaDB runtime
  startup.
- `docs/specs/ownerless-cross-process-concurrency/specs.md` requires a probe in
  the database directory and explicit rejection for unsupported or unproven
  filesystems.
- The fast wait backend is performance evidence. Correctness requires the
  shared mapping, byte-range lock, release-on-exit, grow/remap, wait/wake, and
  process-identity primitives.

## Design

Add an internal `mylite_ownerless_probe_directory()` API that creates the probe
root under the requested directory and reuses the existing primitive checks.
Keep `mylite_ownerless_probe_platform()` as the `/tmp` host/platform probe.

For `MYLITE_OPEN_OWNERLESS_RW` and `MYLITE_OPEN_SHARED_READONLY`, `mylite_open`
now:

1. validates and prepares the database directory,
2. checks `concurrency/mylite-ownerless-platform.meta` for a successful proof
   bound to the database directory's current device id,
3. runs the directory probe when the proof is absent or for a different device,
4. writes a new proof after the required primitives pass, and
5. rejects ownerless mode with `MYLITE_ERROR` before startup lock acquisition
   and MariaDB runtime startup if the probe fails.

Ordinary exclusive read/write opens do not run the ownerless directory probe.
This keeps the normal non-ownerless startup path close to trunk and avoids
running fork-heavy primitive probes on repeated ownerless opens for an already
proven directory.

## Scope And Non-Goals

In scope:

- Database-directory primitive probe API.
- Ownerless open-time gate for read/write and shared read-only ownerless modes.
- Device-bound proof marker to avoid repeated full probes on the same directory.
- Hook-only forced probe failure coverage.

Out of scope:

- Claiming support for network filesystems.
- A public API for path-specific capability queries.
- Windows or macOS-specific backend implementation.
- SQL-level table-lock fault injection.

## Compatibility Impact

`mylite_capabilities()` remains build/profile-level capability reporting. A
specific ownerless open can still fail when the target database directory cannot
prove the required filesystem primitives.

Existing ownerless-compatible local filesystems continue to open. Unsupported
or unproven filesystems fail earlier and with a MyLite ownerless platform error.
Ordinary exclusive read/write opens are unchanged.

## Directory And Lifecycle Impact

A successful ownerless directory probe writes
`concurrency/mylite-ownerless-platform.meta`:

```text
format=1
database_device=<st_dev>
required_primitives=1
process_identity=1
```

The proof is advisory and can be rebuilt. If the file is absent, unreadable,
malformed, or bound to a different device id, the next ownerless open probes the
database directory again.

## Test Plan

- Extend `mylite_ownerless_primitives_test` to call
  `mylite_ownerless_probe_directory()` against a temporary directory and assert
  the required primitives, including process identity, pass.
- Add hook-only forced probe failure through
  `MYLITE_OWNERLESS_TEST_PROBE_FAIL`, compiled only in
  `MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS` builds.
- Add `libmylite.ownerless-platform-probe-failure` under the
  `ownerless-test-hooks` preset to prove ownerless read/write and shared
  read-only opens reject a forced failed probe while ordinary read/write still
  opens.
- Run focused primitive, hook negative-proof, and ownerless open verification,
  plus `git diff --check`.

## Acceptance Criteria

- Directory probes use files created under the requested directory.
- Ownerless opens reject failed required-primitives probes before runtime
  startup.
- Ordinary exclusive read/write opens do not run or honor the forced
  ownerless-probe failure hook.
- Successful ownerless opens write a device-bound proof and later opens can
  skip the full probe while the database directory remains on the same device.
- Cached ownerless platform proofs without `process_identity=1` are treated as
  stale and reprobed before ownerless mode is accepted.

## Risks

- The proof marker is intentionally simple. It catches database-directory moves
  across devices, but it does not claim deep network-filesystem safety.
- First ownerless open on a directory still pays the full primitive probe cost.
  That is acceptable because repeated opens use the proof marker and ordinary
  opens stay outside the ownerless gate.
