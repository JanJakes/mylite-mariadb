# Locking And Concurrency

## Goal

Add a safe directory-level lock for durable MyLite database directories and
validate that a second process cannot start an embedded runtime over a directory
that another process already owns.

## Non-Goals

- Do not implement read-only opens yet.
- Do not claim cross-process multiple-reader support before read-only engine
  startup is implemented and tested.
- Do not claim cross-process concurrent writers; unsafe opens must fail or wait.
- Do not change MariaDB storage-engine lock semantics.
- Do not perform size profile hardening.

## Design

Durable database directories get a stable lock anchor:

```text
app.mylite/
  mylite.meta
  mylite.lock
  datadir/
  tmp/
  run/
```

`mylite.lock` is a MyLite-owned advisory lock file. It is not application data.
It may remain after clean or unclean shutdown so future opens have a stable path
to lock before touching `run/` or starting MariaDB.

The first process handle for a durable path opens `mylite.lock` and takes an
exclusive non-blocking advisory lock. The lock is held until the final handle in
that process closes. Additional handles in the same process share the existing
runtime and do not take another file lock.

`mylite_open_config.busy_timeout_ms` controls how long `mylite_open()` waits
for the directory lock:

- `0`: fail immediately with `MYLITE_BUSY`.
- `>0`: poll until the timeout expires, then return `MYLITE_BUSY`.

After the lock is acquired, MyLite may replace stale inactive `run/` state and
start MariaDB. If the lock cannot be acquired, MyLite must not remove `run/`,
clear `tmp/`, or start the embedded runtime.

`:memory:` keeps the existing temporary runtime behavior and does not use
`mylite.lock`.

## Test Plan

1. Add `libmylite.embedded-locking-concurrency`.
2. Open a durable directory in a child process, hold it open, and verify the
   parent receives `MYLITE_BUSY` when opening the same directory.
3. Verify the failed parent open does not remove the child's live `run/`.
4. Let the child close cleanly and verify the parent can open the directory.
5. Let a child process exit without `mylite_close()` and verify the parent can
   reopen the directory after the OS releases the advisory lock.

## Acceptance Criteria

- Cross-process concurrent durable opens fail with `MYLITE_BUSY` by default.
- The lock file stays inside the MyLite database directory.
- A failed lock attempt leaves live runtime state untouched.
- Clean close and unclean process exit both allow a later open.
- Compatibility docs describe exclusive cross-process directory ownership and
  keep multiple-reader and concurrent-writer support planned.
