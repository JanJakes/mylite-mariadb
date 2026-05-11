# Engineering standards

These standards keep MyLite embeddable, maintainable as a MariaDB fork, and
honest about single-file semantics.

## Source authority

- MyLite is MariaDB-derived. Use MariaDB source and official MariaDB
  documentation for the selected base line as the implementation authority.
- The initial target base is MariaDB 11.8 LTS unless a later decision changes
  the branch.
- Record exact upstream refs in substantial research and implementation docs.
- Use MySQL behavior as compatibility evidence when evaluating drop-in API or
  application behavior.

## Upstream-derived code

- Keep MariaDB-derived files close to upstream.
- Prefer narrow, documented patches over broad rewrites.
- Preserve upstream style in upstream files unless the local change is isolated
  in a clearly MyLite-owned module.
- Avoid formatting churn in imported source.
- Explain non-obvious fork deltas in comments only when the code would otherwise
  be misleading.

## First-party code

- Public functions use the `mylite_*` prefix.
- Public constants and macros use the `MYLITE_*` prefix.
- Public handles are opaque lowercase C types:

  ```c
  typedef struct mylite_db mylite_db;
  typedef struct mylite_stmt mylite_stmt;
  ```

- Public functions return integer status codes unless they are simple infallible
  accessors.
- Detailed diagnostics are retrieved from the relevant handle, not process
  global state.
- Public structs are exposed only when their layout is intentionally stable.
  Growable public structs should include a size or version field.
- Public headers should be C++ compatible where practical.

## Public API

- The primary API is `libmylite`, not `MYSQL *`.
- MariaDB C API compatibility can be an adapter later, but it must not define
  the core lifetime model.
- Opening a database must be a file ownership operation.
- Closing a database must release handle-owned resources predictably.
- The API should expose warnings, SQLSTATE, numeric MariaDB diagnostics, affected
  rows, insert ids, metadata, and side effects where relevant.

## Symbol visibility

- First-party library symbols are hidden by default.
- Public ABI functions are exported with an explicit API macro.
- Internal functions are not exported.
- Static helpers remain `static` unless a real module boundary requires
  otherwise.

## Single-file invariants

- The primary database asset is one `.mylite` file.
- Persistent `.frm`, `.ibd`, `.MAI`, `.MAD`, `aria_log.*`, `ib_logfile*`,
  binlog, relay log, and plugin sidecars are incompatible with the final product
  shape.
- MyLite-owned rollback journal, WAL, shared-memory, lock, and temporary spill
  files may be used when their lifecycle is documented and tested.
- Cross-process write concurrency must not be claimed until locking and recovery
  are designed and tested.

## Storage engine work

- Prefer a MyLite-owned static storage engine over intercepting every file
  operation from existing engines.
- Use MariaDB handler and table-discovery APIs where they fit.
- Store table definitions, catalog state, row storage, indexes, transaction
  metadata, and recovery state in the `.mylite` file.
- A virtual datadir container can be used only as an explicitly temporary
  compatibility experiment.
- Do not treat table discovery as complete until the DDL write path has been
  tested for `CREATE`, `ALTER`, `DROP`, and `RENAME` without durable `.frm`
  sidecars.

## Build system

- Use target-based CMake.
- Do not use in-source builds.
- Keep embedded-only build configuration reproducible.
- Do not hardcode global optimization flags where CMake build types or target
  properties are the right mechanism.
- Track binary-size-sensitive choices with measured artifacts once builds exist.
- Avoid pulling daemon-only targets, unrelated storage engines, or rare
  optional plugins into the default library profile.

## Dependencies and licensing

- MyLite is GPL-2.0 because it contains MariaDB-derived server code.
- SQLite-inspired file or API ownership does not imply SQLite-like licensing.
- Review public naming, packaging, and no-affiliation notices before release.
- Every new dependency needs a license review, version pin, update process, and
  rationale.
- Keep dependencies lean. Embeddability and binary size are core product
  constraints.
- Vendored source should remain pristine where possible. Local patches must be
  explicit and documented.

## Error handling and ownership

- Public APIs validate required pointer arguments.
- Invalid public API usage returns a misuse-style result, not an assertion.
- Allocation failure returns a distinct no-memory result.
- Cleanup functions should tolerate `NULL` unless there is a strong documented
  reason not to.
- Handles returned by MyLite are owned by the caller and released with
  matching MyLite cleanup functions.
- Heap memory returned to callers must be freed through a MyLite API once
  such allocation APIs exist.

## State and threading

- Minimize mutable process-global state.
- File-owned runtime state should be attached to explicit database or shared
  file-runtime objects.
- Session state, diagnostics, warnings, SQL mode, default schema, and temporary
  state belong to handles or explicit contexts.
- Preserve the full write-concurrency goal in storage and runtime designs.
- Do not claim cross-process or multi-writer behavior until locking, recovery,
  and tests prove it.

## Tests and verification

- Add tests with implementation work.
- Compare behavior against MariaDB for compatibility-sensitive SQL behavior.
- Use MySQL behavior as additional compatibility evidence for drop-in API and
  application workloads.
- Exercise embedded lifecycle, repeated open/close, file locking, crash recovery,
  catalog discovery, and unsupported-server-surface diagnostics.
- Do not skip failing tests or static checks. Fix the underlying issue.
- Record commands and environment details when results are used as design
  evidence.

## Documentation

- Substantial work should have a slice spec under `docs/specs/<slice-slug>/`.
- Use the pre-implementation checklist before starting substantial code slices.
- Update architecture and API docs when implementation changes design.
- Update compatibility matrices when implementation changes supported behavior,
  storage-engine routing, protocol behavior, or unsupported surfaces.
- Document deliberately unsupported behavior.
- Prefer concise source-linked notes over broad prose that cannot be verified.
- Keep workflow docs and local skills aligned when a new required slice exists.

## Commits

- Keep commits atomic and reviewable.
- Use concise imperative present tense subjects.
- Include upstream refs, source links, or rationale in the body when useful.
- Keep upstream import/rebase commits separate from MyLite patches.
- Do not add generated assistant or co-author footers.
