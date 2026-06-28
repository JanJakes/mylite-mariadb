# Ownerless Column Change Live Recovery

## Summary

MyLite now treats bounded real `ALTER TABLE ... CHANGE COLUMN` DDL as a
recoverable ownerless dictionary boundary when MariaDB has completed the native
table-definition mutation but the writer dies before ownerless dictionary
finish.

The supported slice is intentionally narrow:

- one stored-column `CHANGE COLUMN old_name new_name definition` clause,
- no generated columns on the table,
- no placement, index, constraint, `AUTO_INCREMENT`, or multi-action clause,
- exact optional `ALGORITHM=COPY, LOCK=EXCLUSIVE` tail after the column clause.

## Recovery Behavior

The ownerless classifier records
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_CHANGE_COLUMN` before native
execution when the source column exists, the target column is absent, and the
table has no generated columns. The recovery kind uses the same native
file-operation checkpoint lane as other real column ALTERs, so a live peer keeps
the marker durable until the final no-live drain can checkpoint native state.

Focused hook coverage kills both:

- `ALTER TABLE ... CHANGE COLUMN note changed_note ...`
- `ALTER TABLE ... CHANGE COLUMN note changed_note ..., ALGORITHM=COPY, LOCK=EXCLUSIVE`

after native success and before ownerless dictionary finish. Recovery verifies
old-column rejection, new-column width/default metadata, retained rows, widened
post-recovery writes, marker retention while another ownerless peer remains
live, marker drain after peer release, ownerless/native reopen, and forced
`.shm` rebuild.

## Remaining Work

Generated-column tables, same-name `CHANGE COLUMN` used as a `MODIFY` spelling,
placement clauses, multi-action column ALTERs, broader online option orders,
copy-lock rename if MariaDB accepts it, and randomized external DDL oracle
stress remain outside this slice.
