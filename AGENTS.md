# Agent guidance

MyLite is a GPL-2.0-only MariaDB fork project. Its product goal is an
embedded MariaDB-derived library with SQLite-like file ownership semantics:
open one primary `.mylite` file, execute MariaDB SQL in-process, close the
file, and keep any journals, WAL files, locks, or temporary spill files under
MyLite's documented lifecycle.

Read [README.md](README.md) before implementation work. For substantial work,
also read the relevant design documents:

- [docs/architecture/engineering-standards.md](docs/architecture/engineering-standards.md)
- [docs/architecture/pre-implementation-checklist.md](docs/architecture/pre-implementation-checklist.md)
- [docs/architecture/single-file-storage.md](docs/architecture/single-file-storage.md)
- [docs/api/libmylite-c-api.md](docs/api/libmylite-c-api.md)
- [docs/research/mariadb-source-analysis.md](docs/research/mariadb-source-analysis.md)
- [docs/ROADMAP.md](docs/ROADMAP.md)

## Project goals

- **In-process:** Expose `libmylite`; do not require a daemon, socket, or
  network handshake.
- **Single primary file:** Expose one primary `.mylite` database file, with
  documented MyLite-owned companion files allowed for recovery, locking, or
  temporary spill.
- **MariaDB semantics:** Inherit MariaDB parser, analyzer, optimizer,
  execution, diagnostics, data types, collations, and DDL behavior where
  practical.
- **Small embedded profile:** Omit daemon-only services, unrelated storage
  engines, and rare optional features from the default library build.
- **Upstream discipline:** Keep MariaDB-derived changes narrow, documented, and
  rebased deliberately.

## Communication

- Be direct, clear, accurate, and concise.
- Explain non-obvious engineering choices.
- If uncertain, say so and identify the missing evidence.
- Do not agree with a suspected issue until it has been investigated.

## Engineering principles

- Treat MariaDB source and official MariaDB documentation for the selected base
  line as the compatibility authority. MySQL behavior is useful context, not the
  normative source for this project.
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
- Preserve useful write concurrency where it can be implemented safely.

## Workflow

Substantial work is organized as engineering slices, not individual SQL
compatibility features. Examples:

- embedded bootstrap and lifecycle,
- `libmylite` open/close API,
- storage-engine discovery,
- DDL metadata routing,
- single-file catalog,
- crash recovery,
- minimal build profile,
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

Compatibility is inherited first from MariaDB, then constrained by the embedded
single-file product shape. Unsupported server-oriented surfaces must be explicit
rather than accidental, including:

- network server behavior,
- replication and binlog,
- dynamic plugins,
- external durable engine files,
- persistent `.frm` metadata sidecars,
- server users and authentication,
- cross-process write concurrency claims before locking and recovery are
  designed and tested.

A detailed compatibility matrix can be added once implementation begins. Early
work should track engineering slices and MariaDB subsystem risks instead of
cataloging every SQL feature.

## Git guidelines

- Make atomic commits with concise imperative subjects.
- Include a brief body when the rationale or source reference matters.
- Keep upstream-sync commits separate from MyLite patch commits.
- Do not add generated assistant or co-author footers.
- Do not rewrite shared history unless explicitly requested.
