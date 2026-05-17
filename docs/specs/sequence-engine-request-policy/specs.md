# Sequence Engine Request Policy

## Problem

The virtual `SEQUENCE` storage engine is disabled in MyLite's embedded profile,
but explicit table DDL can still request it with `ENGINE=SEQUENCE` or MariaDB's
no-equals `ENGINE SEQUENCE` spelling. That should be a tested MyLite policy
failure before MariaDB execution and catalog publication.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/sequence/sequence.cc` implements a visible `SEQUENCE`
  storage engine that auto-discovers generated tables such as `seq_1_to_10`.
- `mariadb/sql/sql_yacc.yy` parses `ENGINE_SYM opt_equal ident_or_text`, so
  both `ENGINE=SEQUENCE` and `ENGINE SEQUENCE` are valid table-option
  spellings.
- The existing sequence storage-engine trim disables `PLUGIN_SEQUENCE=NO`, and
  the unsupported-engine policy now rejects known unsupported engine names
  before MariaDB execution.

## Design

- Keep `SEQUENCE` out of the supported engine allowlist.
- Cover explicit `SEQUENCE` engine requests through the same MyLite-owned
  unsupported-engine policy as other unsupported table engines.
- Preserve ordinary user tables with names such as `seq_1_to_10`; this policy
  only applies to explicit `ENGINE` table options.

## Compatibility Impact

Applications that explicitly request MariaDB's virtual `SEQUENCE` engine remain
unsupported in the core embedded profile. Ordinary MyLite tables whose names
look like MariaDB virtual sequence tables remain valid catalog-backed tables.

## DDL Metadata Routing Impact

Failed `SEQUENCE` engine requests do not create MyLite catalog records and do
not mutate requested/effective engine metadata on existing routed tables.

## Test Plan

- Direct execution rejects `ENGINE=SEQUENCE` and `ENGINE SEQUENCE`.
- Prepared execution rejects `ENGINE=SEQUENCE`.
- Storage-engine smoke verifies no catalog records are published for failed
  `SEQUENCE` engine requests and an existing routed table keeps its metadata.
- Existing virtual sequence table-name coverage continues to prove ordinary
  `seq_*` user tables remain valid.

## Acceptance Criteria

- Explicit `SEQUENCE` engine requests return a MyLite-owned error with no
  MariaDB errno.
- Failed requests do not create or mutate MyLite catalog records.
- Compatibility docs distinguish explicit `ENGINE` requests from ordinary
  user-created `seq_*` table names.
