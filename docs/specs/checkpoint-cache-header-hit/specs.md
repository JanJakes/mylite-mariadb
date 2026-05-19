# Checkpoint Cache Header Hit

## Problem

Fresh sampling after read-startup syscall trimming still shows repeated
checkpoint snapshot startup spending time in two fixed page reads. On a hot
read loop the first read compares page `0` against the cached header. When that
header is byte-identical, the current code still reads the catalog root page
only to compare it against the cached catalog page.

That second read is redundant for committed MyLite snapshots. Catalog root page
identity and catalog generation are encoded into the checksummed header page,
and durable catalog changes publish a new header.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::initialize_read_statement()` already
  validates cached read-file identity before reusing an unlocked file handle.
- `read_cached_checkpoint_snapshot()` reads the raw header page and compares it
  to the thread-local checkpoint cache.
- When the raw header matches, the function reads the cached catalog root page
  and compares bytes before copying the decoded checkpoint cache.
- `encode_header_page()` includes the catalog root page, catalog generation,
  free-list root, page count, and header checksum. Catalog publication updates
  the header when the catalog root or generation changes.
- A different database file can legitimately have byte-identical header fields
  and different catalog bytes, so header-only reuse must also require matching
  file identity.

## Design

- Keep the raw header page read as the external-writer detection point for hot
  read statements.
- Carry device/inode identity on read statements and checkpoint cache entries.
- On a matching file identity plus byte-identical header-page match, copy the
  cached decoded header/catalog snapshot directly into the read statement.
- Keep the existing full snapshot validation path when the raw header differs,
  including catalog root read and validation.
- Reuse the read statement's known file identity when putting an unlocked read
  handle back into the thread-local file cache, avoiding a second `fstat()` at
  read-statement close.

## Compatibility Impact

No SQL or API behavior changes. Cross-process committed changes still change
the durable header page before the cached snapshot can be reused, and path
replacement with an independent file cannot reuse another file's cached catalog
snapshot.

## Single-File And Lifecycle Impact

No file-format, lock, journal, or companion-file change.

## Storage-Engine Routing Impact

All durable routed engines using MyLite read statements benefit, including
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted/default engines, and
`ENGINE=MYLITE`.

## Binary-Size And Dependency Impact

Small first-party C deletion. No new dependency.

## Tests And Verification

- Reuse storage tests that cover catalog changes, reopen, journal recovery, and
  cached read-statement replacement.
- Run routed storage-engine smoke coverage.
- Run the local performance baseline to compare point and secondary exact
  reads.
- Run formatting and whitespace checks.

## Acceptance Criteria

- Same-file header-cache hits do not read the catalog page only to compare it
  against the cached catalog bytes.
- Replacement files with byte-identical headers do not reuse stale checkpoint
  cache entries.
- Header-cache misses still perform full checkpoint snapshot validation.
- Existing storage and storage-smoke tests pass.
- The local benchmark does not regress read timings.

## Risks And Open Questions

- This relies on the existing MyLite invariant that durable catalog changes
  publish a header change within the same file identity. A future durable
  data-generation field would make this relationship more explicit for row-page
  cache invalidation too.
