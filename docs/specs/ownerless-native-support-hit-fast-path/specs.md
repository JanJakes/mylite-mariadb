# Ownerless Native-Support Hit Fast Path

## Context

Ownerless visible-fast row-list inserts retain shared page-write ownership for
native-support pages that the existing native-support elision predicate accepts.
After the page is retained, repeated undo and rollback-segment mini-transactions
usually only need to recognize that the transaction already owns that
native-support page and return without acquiring another page-write lock.

The large-row ownerless attribution probe at the previous head showed the
remaining non-empty-table bulk path was still dominated by native undo reporting:
the reduced 100-row statement sample reported ownerless bulk at a `0.2966`
ownerless/ordinary rows ratio, later statements at `0.3009`, and remaining
undo-report MTR commit at `0.508 ms/statement` versus ordinary `0.029`. The same
sample reported `202.000` native-support held-page hits per later statement and
`100.000` elided native-support pages, so repeated held-native-support
membership is a measurable hot-path shape even though it is not the whole undo
cost.

## Design

The change keeps the existing lock and publication contract:

- a native-support page can be treated as already held only when the
  transaction-local held native-support vector says so;
- the vector is still populated only after an actual page-write acquisition and
  the existing native-support elision predicate;
- rollback-segment and undo history-proof pages still publish through the
  existing proof path;
- user/index pages, transaction-deferred dirty-page publication, redo
  completion, checkpoints, WAL format, and recovery are unchanged.

The hot-path reduction is limited to classification order. Once
`ownerless_page_write_enter()` has proved that page-write hooks are active, the
page is lockable, and the statement is not a select or lock-only transaction, it
checks whether the current transaction already holds the native-support page
before running the broader transaction-release classification. The same
positive membership also lets `ownerless_page_write_should_prepare()` decline
the later enter hook for repeated page-latch prepare paths.

This is deliberately not a non-empty-table undo elision. Reusing
`TRX_UNDO_EMPTY` after the first statement would not preserve rollback-to-
statement or duplicate-key rollback semantics for a non-empty table, so that is
not claimed here.

## MariaDB Source References

- `mariadb/storage/innobase/mtr/mtr0mtr.cc`: `mtr_t::ownerless_page_write_enter()`
  and `mtr_t::ownerless_page_write_should_prepare()` perform the ownerless
  page-write classification for buffer-page latches.
- `mariadb/storage/innobase/trx/trx0trx.cc`: `trx_t` keeps authoritative
  modified, dirty, and held native-support page vectors plus last-hit caches.
- `mariadb/storage/innobase/trx/trx0rec.cc`: `trx_undo_report_row_operation()`
  remains the per-row undo reporting boundary; this slice does not change its
  undo-record semantics.

## Tests

Focused SQL coverage should continue to prove:

- native-support WAL elision and held native-support page-write hits;
- visible-fast multi-row inserts still use transaction-deferred page publication
  and publish native-support proof records;
- rollback/duplicate-key behavior for ownerless default-checked bulk insert
  remains row-correct.

Performance verification should use the production embedded probe with
`MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100` and
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` to compare the ownerless bulk
ratio, remaining undo-report MTR commit time, and held native-support hit
counters before and after the change.

The first local verification kept `2.000` page versions and `2.000` published
native-support pages per remaining statement. Enter-path held native-support hit
accounting moved from `202.000` to `102.000` per remaining statement because
the prepare path now returns before the later enter hook for already-held
native-support pages. The stats-enabled sample moved ownerless bulk
`mysql_query()` from `3.431 ms/statement` to `2.912` and moved the
ownerless/ordinary bulk rows ratio from `0.2966` to `0.4640`. The stats-off
5000-row production probe reported ownerless bulk at `34064.01 rows/s` and a
`0.3391` ratio. These samples do not close the larger remaining undo-report MTR
commit target.
