# Ownerless Native Reopen Redo Recovery

## Problem

Repeated ownerless routine-policy selectors exposed an intermittent ordinary
native read/write reopen abort after ownerless activity:

```text
InnoDB: Missing FILE_CHECKPOINT(...) at ...
InnoDB: Log scan aborted
```

The failing open was `MYLITE_OPEN_READWRITE`, not ownerless mode. The database
had a valid MariaDB redo startup prefix and a valid
`concurrency/mylite-redo-header.bin` backup, but the ownerless page-version WAL
had already been reclaimed. The ordinary native path therefore did not install
ownerless page-read hooks and also did not enable the ownerless
uncheckpointed file-operation recovery flag before InnoDB startup.

## Design

Keep the fast ordinary path narrow:

- Ordinary native opens still install ownerless InnoDB hooks only when retained
  page-version WAL payload records must be readable during startup.
- The InnoDB uncheckpointed file-operation recovery flag is now separate from
  full hook installation.
- Non-read-only native startup enables that recovery flag only when one of
  these durable ownerless evidence sources exists:
  - retained page-version WAL payload records,
  - the native file-op checkpoint-needed bit in `mylite-concurrency.ckpt`,
  - a valid `mylite-redo-header.bin` backup whose saved startup prefix passes
    the existing redo-header/checkpoint validation and whose recorded redo size
    is within the bounded tolerance.
- Successful startup in those evidence-bearing cases refreshes
  `mylite-redo-header.bin` from the captured valid startup prefix so later
  bounded retries keep a current restore point.

This does not change MariaDB redo format and does not make missing
`FILE_CHECKPOINT` acceptable for arbitrary native corruption. The InnoDB
tolerance still applies only at the existing clean-EOF/no-corrupt-FS recovery
boundary and only when MyLite ownerless evidence has armed that mode.

## Compatibility Impact

No SQL syntax or public C API change. Ordinary native read/write verification
after ownerless DDL-policy handoffs can recover the ownerless suppressed
checkpoint boundary even when there are no page-version WAL payload records
left to replay.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the embedded preset.
- Reproduce the pre-fix failure with repeated `routine-policy` and
  `routine-execution-policy` selector runs.
- Rerun the same repeated selector loop after the fix.
- Run focused selectors:
  `routine-policy`, `routine-execution-policy`, `native-reclaim`,
  `native-file-op-marker-drain`, and `statement-checkpoint-scheduling`.
- Run the embedded ownerless SQL CTest shards.
- Run the ownerless hook subset and ownerless stress selector used by the
  surrounding native reclaim slices.
