# Ownerless Column Rename Copy-Lock Live Recovery

## Summary

MariaDB accepts `ALTER TABLE ... RENAME COLUMN ..., ALGORITHM=COPY,
LOCK=EXCLUSIVE`. MyLite now treats that exact spelling as the same bounded
ownerless dictionary recovery class as ordinary stored-column `RENAME COLUMN`.

## Recovery Behavior

The rename classifier accepts either a semicolon-only tail or the exact
`, ALGORITHM=COPY, LOCK=EXCLUSIVE` tail. It still requires a single
`RENAME COLUMN old_name TO new_name` clause, an existing source column, and an
absent target column before native execution.

Focused hook coverage kills the copy-lock rename writer after MariaDB completes
the native rename but before ownerless dictionary finish. Recovery verifies:

- old-column metadata and reads are absent,
- renamed-column metadata/defaults and retained rows are present,
- post-recovery writes through the renamed column succeed,
- the native file-operation marker remains set while a live peer is open,
- the marker drains after peer release,
- ownerless/native reopen and forced `.shm` rebuild keep the recovered state.

## Remaining Work

Generated-column rename under exact copy-lock options, multi-action column ALTER
lists, broad online option order matrices, and external randomized DDL oracle
stress remain outside this focused slice.
