# Engineering Standards

These standards keep MyLite embeddable, compatible with MariaDB semantics, and
maintainable as a focused MariaDB fork.

## Source Authority

- Target base: MariaDB 11.8 LTS.
- Initial import ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Use MariaDB source and official MariaDB documentation as the implementation
  authority.
- Use MySQL behavior as compatibility evidence for drop-in application behavior,
  not as the primary source of truth.
- Record exact upstream refs when a slice depends on MariaDB internals.

## Fork Discipline

- Keep upstream-derived files close to upstream.
- Keep mechanical upstream imports separate from MyLite changes.
- Prefer narrow patches over broad rewrites.
- Preserve upstream style in MariaDB-derived files.
- Put first-party code behind clear `mylite_*` and `MYLITE_*` names.
- Explain fork deltas in code only when the local behavior would otherwise be
  misleading.

## Public C API

- The primary API is `libmylite`, not `MYSQL *`.
- Public functions use the `mylite_*` prefix.
- Public constants and macros use the `MYLITE_*` prefix.
- Public handles are opaque lowercase C types:

  ```c
  typedef struct mylite_db mylite_db;
  typedef struct mylite_stmt mylite_stmt;
  ```

- Public functions return stable MyLite result codes.
- MariaDB errno, SQLSTATE, warnings, affected rows, insert ids, and metadata are
  exposed through MyLite APIs where applications depend on them.
- Public structs are exposed only when their layout is intentionally stable;
  growable structs include a size field.
- Public headers are C++ compatible.
- Raw `MYSQL *` access belongs in an optional adapter.

## Storage

- The primary database asset is one `.mylite` file.
- MyLite implements a static storage engine for durable application tables.
- Persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`,
  `ib_logfile*`, binlog, relay log, and plugin-owned table files are outside
  the final storage model.
- MyLite-owned journal, WAL, shared-memory, lock, and temporary spill companions
  are allowed when their lifecycle is documented and tested.
- DDL support is not complete until `CREATE`, `ALTER`, `DROP`, and `RENAME`
  publish MyLite catalog metadata without durable metadata sidecars.
- Storage and locking designs preserve the full write-concurrency goal.

## Build

- Use target-based CMake.
- Do not use in-source builds.
- Keep embedded build configuration reproducible.
- Keep daemon-only targets, unrelated durable engines, and rare optional
  plugins out of the default library profile.
- Track binary-size-sensitive choices with measured artifacts.
- Every new dependency needs a license, size, and maintenance rationale.

## State, Ownership, And Errors

- Minimize mutable process-global state.
- File-owned runtime state belongs to explicit database or shared file-runtime
  objects.
- Session state, diagnostics, warnings, SQL mode, default schema, and temporary
  state belong to handles or explicit contexts.
- Public APIs validate required pointer arguments.
- Invalid public API usage returns a misuse-style result, not an assertion.
- Allocation failure returns a distinct no-memory result.
- Cleanup functions tolerate `NULL`.
- Heap memory returned to callers is released through a MyLite API.

## Tests

- Add tests with implementation work.
- Compare compatibility-sensitive SQL behavior against MariaDB.
- Add MySQL evidence when application drop-in behavior depends on MySQL-specific
  expectations.
- Exercise embedded lifecycle, open/close lifecycle, catalog discovery,
  sidecar detection, file locking, transaction boundaries, crash recovery, and
  unsupported server surfaces.
- Do not skip failing tests or static checks. Fix the underlying issue.
- Record commands and environment details when results are used as design or
  compatibility evidence.

## Documentation

- Keep architecture docs aligned with implemented design.
- Keep [../COMPATIBILITY.md](../COMPATIBILITY.md) aligned with supported and
  deliberately unsupported behavior.
- Use the pre-implementation checklist before starting substantial code slices.
- Use slice specs for substantial implementation work that needs source-linked
  design before code.
- Prefer concise source-linked notes over broad prose.

## Commits

- Keep commits atomic and reviewable.
- Use concise imperative present-tense subjects.
- Include upstream refs, source links, or rationale in the body when useful.
- Keep upstream import/rebase commits separate from MyLite patch commits.
- Do not add generated assistant or co-author footers.
