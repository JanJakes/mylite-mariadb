# Ownerless Native Support Record Marker

## Problem Statement

Ownerless checkpoint proof replay must distinguish native-support page-version
records from snapshot-sensitive user records before deciding whether retained
page-version WAL can be reclaimed. Before this slice, proof replay re-read and
decoded each candidate page-log payload to classify InnoDB page type, even
though the ownerless publish hook already knows when it is appending a
native-support page image.

This slice adds a page-log metadata marker for native-support records so proof
replay and oldest-snapshot boundary checks can skip payload decoding when the
marker is present. The marker does not change payload bytes, page checksums,
record ordering, checkpoint sync ordering, or the existing history-proof
requirement.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` already classifies
  native-support page types for ownerless page-publication stats and WAL
  elision policy.
- `packages/libmylite/src/database.cc` classifies the copied page image in
  `ownerless_innodb_page_publish_hook()` before choosing snapshot-boundary and
  page-index publication behavior.
- `packages/libmylite/src/database.cc` previously used
  `ownerless_page_log_record_is_native_support_state()` during native
  checkpoint proof collection, which reads the page-log payload and decodes the
  page image just to classify native-support state.
- `packages/libmylite/src/ownerless_page_log.cc` already has metadata flags
  for snapshot-boundary and external-snapshot-lineage records, and retained
  checkpoint rewrites preserve metadata flags while converting retained delta
  payloads back to standalone records.

## Design

- Add `MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE` as a page-log
  record metadata flag.
- Add `MYLITE_OWNERLESS_PAGE_LOG_APPEND_NATIVE_SUPPORT_STATE` as an append
  option used by ownerless publication when the publish hook has already
  classified the page as native-support state.
- Preserve the marker through direct appends, append-session appends,
  external-snapshot-lineage appends, and retained-record checkpoint rewrites.
- Add `mylite_ownerless_page_log_record_is_native_support_state_at()` so
  checkpoint proof code can test the record header before decoding payload.
- Keep fallback payload classification for unmarked records, preserving
  compatibility with page-log records written before this marker existed.

## Compatibility And Storage Impact

The marker is metadata only. It does not change SQL behavior, page-log payload
format selection, page checksums, durable page images, or page-index
publication. Older unmarked records remain readable because proof replay falls
back to the old payload-based classifier when the marker is absent.

This is not a replacement for the rollback-segment/undo native history proof
and does not claim broader redo/checkpoint recovery completion.

## Test Plan

- Primitive page-log coverage appends a marked native-support record, verifies
  the direct record-header query, checkpoints with a retained rewrite, verifies
  the marker is preserved, and reads the retained page byte-identically.
- Existing native-support, history-proof, checkpoint, and ownerless SQL
  selectors continue to cover producer integration.

## Acceptance Criteria

- Marked native-support page-log records can be detected without payload
  decoding.
- Retained checkpoint rewrites preserve the marker.
- Unmarked records still use the legacy payload classifier.
- Page-version WAL payload bytes, checksums, ordering, and recovery semantics
  are unchanged.
