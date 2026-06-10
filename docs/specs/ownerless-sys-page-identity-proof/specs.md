# Ownerless SYS Page Identity Proof

## Problem

The ownerless native-support attribution split proved that the measured
`trx_system` bucket record in the reduced ownerless autocommit insert path is
`FIL_PAGE_TYPE_SYS`, not the canonical `FIL_PAGE_TYPE_TRX_SYS` page. The old
aggregate therefore identified the wrong next target. The remaining published
system-page record is still a full page-version WAL image, but MyLite cannot
elide it until the page identity and MariaDB source semantics are known.

This slice identifies which `FIL_PAGE_TYPE_SYS` page identity is published or
elided in the measured ownerless write path. It is a proof and attribution
slice only; it does not change page-version WAL retention or native storage
behavior.

## Current Evidence

Local production verification on 2026-06-10 used `build/mariadb-embedded`
with the `MinSizeRel` baseline and `build/php-embedded-prod` with `Release`
first-party artifacts. A reduced stats-enabled probe with `100` ownerless
autocommit inserts reported:

- `1.000` published `FIL_PAGE_TYPE_SYS` pages per insert,
- `0.200` elided `FIL_PAGE_TYPE_SYS` pages per insert,
- first published SYS identity `(space_id=1,page_no=41)`,
- first elided SYS identity `(space_id=1,page_no=41)`,
- `1.000` published SYS undo-tablespace pages per insert,
- `0.200` elided SYS undo-tablespace pages per insert,
- `0.000` published or elided SYS pages in the system tablespace page `3`,
  page `4`, first rollback-segment page `6`, dictionary header page `7`,
  other system-tablespace pages, or non-undo other tablespaces,
- `0.990` published SYS pages per insert and `0.190` elided SYS pages per
  insert belonged to identities other than the first observed identity,
- `49672.960` page-log bytes per insert.

The measured SYS publication is therefore undo-tablespace file-segment/system
state, not system-tablespace change-buffer, rollback-segment, dictionary, or
canonical TRX_SYS page state. A later optimization must still prove why many
undo-space SYS identities remain published instead of elided by the current
rollback-segment-space predicate.

A default stats-off production probe rerun after the identity counters reported
ownerless autocommit at `1163.16 ops/s` versus ordinary autocommit at
`1640.06 ops/s`, ownerless transactional inserts at `1517.34 ops/s` versus
ordinary transactional inserts at `1711.83 ops/s`, and ownerless
active-runtime reconnect at `1.390 ms`. The first stats-off run in the same
environment was noisy and reported a much lower ownerless autocommit rate, so
the rerun is the recorded sanity sample.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_SYS = 6` and `FIL_PAGE_TYPE_TRX_SYS = 7`.
- `mariadb/storage/innobase/include/fsp0types.h` defines system tablespace
  low-page identities: `FSP_IBUF_HEADER_PAGE_NO = 3`,
  `FSP_IBUF_TREE_ROOT_PAGE_NO = 4`, `FSP_TRX_SYS_PAGE_NO = 5`,
  `FSP_FIRST_RSEG_PAGE_NO = 6`, and `FSP_DICT_HDR_PAGE_NO = 7`.
- `mariadb/storage/innobase/srv/srv0start.cc` creates dummy change-buffer
  compatibility pages at pages `3` and `4` for new databases, creates the
  TRX_SYS page separately, and later checks pages `3`, `6`, and `7` as
  `FIL_PAGE_TYPE_SYS` while checking page `5` as `FIL_PAGE_TYPE_TRX_SYS`.
- `mariadb/storage/innobase/ibuf/ibuf0ibuf.cc` still names page `3` as the
  former change-buffer header and page `4` as the former change-buffer root
  for upgrade/removal compatibility.
- `mariadb/storage/innobase/fsp/fsp0fsp.cc`
  `fseg_alloc_free_page_general()` writes `FIL_PAGE_TYPE_SYS` when allocating
  a new file-segment page.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_publish_type_has_native_support()` treats
  `FIL_PAGE_TYPE_SYS` as native-support state, and the existing elision
  predicate can elide native-support pages only in the transaction rollback
  segment's undo tablespace.

## Proposed Design

Keep the previous aggregate and split counters unchanged. Add stats-only
identity attribution for `FIL_PAGE_TYPE_SYS` pages on both the published and
elided ownerless native-support paths:

- total published and elided `FIL_PAGE_TYPE_SYS` counts remain available
  through the existing type split;
- expose the first observed published and elided `FIL_PAGE_TYPE_SYS`
  `(space_id,page_no)` identity as packed and decoded probe fields;
- count how many `FIL_PAGE_TYPE_SYS` events match the first observed identity
  versus later distinct identities;
- add coarse identity class counters for known system tablespace pages
  `3`, `4`, `6`, and `7`, undo-tablespace pages, other system-tablespace
  pages, and other tablespace pages.

These counters are active only when ownerless page-publish stats are enabled.
They must not change the default stats-off write path beyond a page-type branch
inside already-instrumented ownerless publication code.

## Scope And Non-Goals

In scope:

- Identify the `FIL_PAGE_TYPE_SYS` identities present in reduced production
  ownerless autocommit attribution.
- Preserve the historical aggregate counters for log continuity.
- Add focused test coverage that verifies the identity counters account for the
  published and elided `FIL_PAGE_TYPE_SYS` totals.
- Update compatibility and performance docs with the measured identity result.

Out of scope:

- Eliding any additional `FIL_PAGE_TYPE_SYS` page-version WAL records.
- Interpreting the byte-level semantics of the identified page.
- Changing MariaDB page formats, file-segment allocation, change-buffer
  compatibility pages, or rollback-segment behavior.
- Broader DDL/file lifecycle, external MariaDB/RQG stress, or WordPress
  PHPUnit changes.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, or MySQL/MariaDB compatibility behavior
changes. The slice adds diagnostics and focused assertions only.

## Database Directory And Lifecycle Impact

No durable file or directory-layout changes. The new counters are process-local
and reset with the existing ownerless page-publish stats reset.

## Native Storage Impact

No native storage format or recovery behavior changes. Any future elision must
prove that the identified `FIL_PAGE_TYPE_SYS` page image is redundant for peer
visibility, forced `.shm` rebuild, native checkpoint/recovery, and page-log
reclaim before changing publication.

## Build, Size, And Dependency Impact

No new dependencies. The production binary impact is limited to stats counters
and small helper functions in existing ownerless diagnostic code.

## Test And Verification Plan

- Rebuild `build/mariadb-embedded` with the production `MinSizeRel` baseline
  after editing `mariadb/`.
- Rebuild `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run `libmylite.ownerless-single-owner-native-support-page-wal-elision` and
  related focused ownerless selectors under `php-embedded-prod`.
- Run a reduced stats-enabled production embedded performance probe and record
  the first published/elided `FIL_PAGE_TYPE_SYS` identities and identity-class
  counters.
- Run a default stats-off production embedded performance probe as a throughput
  sanity check.
- Run `tools/check-ci-production-builds`,
  `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`,
  `cmake --build --preset format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- The stats-enabled probe shows the first published `FIL_PAGE_TYPE_SYS`
  identity and class for the reduced ownerless autocommit insert path.
- The docs record whether the measured SYS pages are system-tablespace fixed
  pages, undo-tablespace pages, or other tablespace pages.
- Published and elided `FIL_PAGE_TYPE_SYS` identity accounting sums to the
  existing published/elided `FIL_PAGE_TYPE_SYS` totals.
- No additional page-version WAL elision or compatibility behavior change is
  introduced.
- Production CI timing steps remain guarded by production build checks.

## Risks And Unresolved Questions

- The first identity may not be the only identity in broader DDL or multi-row
  workloads; this slice records first identity plus distinct-identity counts,
  not a complete dynamic histogram.
- A `FIL_PAGE_TYPE_SYS` page can represent different MariaDB internals
  depending on space and page number. Identity proof is necessary but not
  sufficient for elision.
- If the identified page is an undo-tablespace file-segment page, the next
  optimization must still prove native history-space flush and recovery cover
  the same visibility boundary.
