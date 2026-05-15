# Agent guidance

MyLite is a GPL-2.0 MariaDB-derived project. Its product goal is an embedded
MySQL/MariaDB drop-in built on a bundled MariaDB foundation: open one primary
`.mylite` file, execute MySQL/MariaDB-oriented workloads in-process, close the
file, and keep any transient MyLite-owned companions under the documented file
lifecycle.

Read [README.md](README.md) before implementation work. For substantial work,
also read the relevant design documents:

- [docs/architecture/engineering-standards.md](docs/architecture/engineering-standards.md)
- [docs/architecture/pre-implementation-checklist.md](docs/architecture/pre-implementation-checklist.md)
- [docs/architecture/mariadb-foundation.md](docs/architecture/mariadb-foundation.md)
- [docs/architecture/storage.md](docs/architecture/storage.md)
- [docs/architecture/compatibility-harness.md](docs/architecture/compatibility-harness.md)
- [docs/api/libmylite-c-api.md](docs/api/libmylite-c-api.md)
- [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)
- [docs/ROADMAP.md](docs/ROADMAP.md)

## Project goals

- **MySQL/MariaDB drop-in:** Support the MySQL/MariaDB API and SQL behavior
  that real applications depend on, with compatibility tracked by tests and a
  coverage matrix.
- **Single file:** Keep durable database state in one portable `.mylite` file,
  with only transient MyLite-owned companions for journals, WAL, locks, shared
  memory, or temporary spill.
- **In-process runtime:** Expose `libmylite` without requiring a daemon,
  socket, or network handshake. Wire-protocol integrations belong around the
  core library, not inside its startup contract.
- **MyLite storage:** Route supported MySQL/MariaDB storage-engine semantics to
  the MyLite single-file storage layer, including InnoDB, MyISAM, Aria, and
  zero-file engines where practical.
- **Write concurrency:** Implement full write concurrency support in MyLite
  storage, and prove claims with locking, transaction, and recovery tests.
- **Small profile:** Keep the minimum necessary MariaDB code for the embedded
  profile and measure size against reproducible builds.

## Communication

- Be direct, clear, accurate, and concise.
- Explain non-obvious engineering choices.
- If uncertain, say so and identify the missing evidence.
- Do not agree with a suspected issue until it has been investigated.

## Engineering principles

- Treat MariaDB source and official MariaDB documentation for the selected base
  line as the implementation authority. Use MySQL behavior as compatibility
  evidence when evaluating drop-in API or application behavior.
- Do not present a virtual datadir wrapper as a final single-file solution.
  It may be useful as a temporary compatibility experiment, but MyLite's
  target architecture is a real single-file runtime.
- Preserve upstream MariaDB style in upstream-derived files unless a local
  subsystem has a stronger documented convention.
- Use `mylite_*` naming and the MyLite C API standards for new public
  first-party code.
- Keep unrelated refactors out of focused changes.
- Account for binary size, startup cost, file-format durability, and fork
  maintenance cost when choosing designs.
- Preserve write concurrency goals in storage designs, and require tests before
  claiming cross-process or multi-writer behavior.

## Workflow

Substantial work is organized as engineering slices, not individual SQL
compatibility features. Examples:

- embedded bootstrap and lifecycle,
- `libmylite` open/close API,
- storage-engine discovery,
- storage-engine routing,
- DDL metadata routing,
- single-file catalog,
- compatibility harness and coverage matrix,
- crash recovery,
- minimal build profile,
- wire-protocol integration,
- upstream import or rebase.

Use the project skills in `.agents/skills/` when they apply:

- `mylite-start-slice` for research and design before code,
- `mylite-implement-slice` for end-to-end implementation,
- `mylite-review-slice` for release-gate review,
- `mylite-upstream-work` for MariaDB import, rebase, or patch-stack work,
- `mylite-dont-stop` when substantial work should continue through natural
  pause points,
- `mylite-work-hard` for ongoing roadmap batches.

## Compatibility management

Compatibility is inherited first from MariaDB, checked against MySQL/MariaDB
application expectations, and constrained by the embedded single-file product
shape. Unsupported server-oriented surfaces must be explicit rather than
accidental, including:

- daemon-owned network server behavior in the core library,
- replication and binlog,
- dynamic plugins,
- external durable engine files,
- persistent `.frm` metadata sidecars,
- server users and authentication,
- cross-process write concurrency claims before locking and recovery are
  designed and tested.

Keep compatibility evidence practical: add or update the coverage matrix when a
slice changes SQL behavior, public API behavior, storage-engine routing,
protocol behavior, unsupported surfaces, or file-lifecycle guarantees.

## Git guidelines

- Make atomic commits with concise imperative subjects.
- Include a brief body when the rationale or source reference matters.
- Keep upstream-sync commits separate from MyLite patch commits.
- Do not add generated assistant or co-author footers.
- Do not rewrite shared history unless explicitly requested.
