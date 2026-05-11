# Pre-Implementation Checklist

Use this checklist before starting a substantial MyLite implementation slice.
The goal is to catch product-shape, compatibility, source, and verification
gaps before code makes them expensive.

## Scope

- Name the roadmap slice and choose a lower-case hyphenated spec slug.
- State the user-visible behavior being added or changed.
- State non-goals, especially server-only surfaces and temporary limitations.
- Identify whether the work is upstream import, upstream-derived patching,
  first-party MyLite code, tests, documentation, or a mix.

## Source Authority

- Record the MariaDB base tag, commit, and relevant branch head if inspected.
- Inspect the relevant MariaDB source paths and functions directly.
- Use official MariaDB documentation as supporting evidence.
- Use MySQL behavior only as compatibility evidence for drop-in application
  expectations.
- Avoid claims based only on file names, outdated docs, or guessed server flow.

## Compatibility

- Identify affected SQL, C API, storage-engine, protocol, or metadata behavior.
- Decide whether the behavior is covered, partial, planned, or out of scope in
  [../COMPATIBILITY.md](../COMPATIBILITY.md).
- Mark unsupported server-oriented surfaces explicitly instead of letting them
  fail accidentally.
- Add MariaDB comparison coverage when behavior is compatibility-sensitive.

## Database Directory Storage

- Keep durable application state in one MyLite-owned database directory.
- Keep supported MariaDB engine files, metadata files, logs, locks, journals,
  and temporary spill inside the MyLite database directory unless a path is
  explicitly documented as transient and MyLite-owned.
- Reject durable files outside the MyLite database directory as final storage.
- Document names, recovery behavior, cleanup behavior, and failure modes for
  native engine files and any MyLite-owned transient paths.
- Preserve MariaDB native storage-engine concurrency guarantees in transaction,
  lock, and directory-lifecycle choices.

## Embedded Runtime And API

- Keep `libmylite` directory-owned open/close as the primary lifetime model.
- Keep daemon startup, sockets, server account management, and authentication
  out of the core startup contract.
- Define repeated initialization, close, cleanup, handle ownership, diagnostics,
  and error behavior.
- Keep raw `MYSQL *` access in an optional adapter, not in the primary API.

## Build, Size, And Fork Hygiene

- Keep mechanical upstream imports separate from MyLite patches.
- Preserve upstream style in MariaDB-derived files.
- Keep first-party names behind `mylite_*` and `MYLITE_*`.
- Record binary-size-sensitive choices with measured artifacts when the slice
  changes the default embedded profile.
- Add a license, size, and maintenance rationale for any new dependency.

## Tests And Verification

- Define the build, unit, integration, compatibility, static-analysis, and size
  checks that will prove the slice.
- Include lifecycle, directory-boundary, metadata, native storage, transaction,
  recovery, locking, and unsupported-surface tests where relevant.
- Do not skip failing tests or static checks. Fix the underlying issue.
- Record commands and environment details when results become design evidence.

## Ready To Implement

Start implementation only when the slice spec names the source refs, design,
compatibility impact, directory-lifecycle impact, test plan, acceptance
criteria, and unresolved risks clearly enough for review.
