---
name: mylite-implement-slice
description: Implement a bounded MyLite engineering slice end to end after design and test expectations exist, including code, MySQL/MariaDB compatibility tests, docs, verification, and self-review. Use only for substantive implementation work, not simple git operations, small docs edits, generic reviews, or status requests.
---

# MyLite implement slice

Implement the slice completely. Do not stop at scaffolding, partial behavior, or
an unfinished task trail when the requested slice can be finished.

## Trigger boundary

Use this skill when the user asks to implement a specific MyLite engineering
slice or materially expand an existing slice through code, tests, docs, and
verification.

Do not use this skill for simple commands, routine commits, amend/push requests,
small docs edits, lightweight bug triage, review-only prompts, or broad batch
work that belongs to `mylite-work-hard`.

Do not infer subagent permission from this skill. Use subagents only when the
active session, tool policy, and user request explicitly allow delegation.

## Source discipline

Use MariaDB source and official MariaDB documentation for the selected base line
as the primary authority. Keep upstream-derived changes narrow and preserve
upstream style. Put new first-party API and native-storage integration code
behind MyLite-owned names and module boundaries. Use MySQL behavior as
compatibility evidence when validating drop-in API or application behavior.

## Workflow

1. Read `README.md`, `AGENTS.md`, `docs/architecture/engineering-standards.md`,
   `docs/architecture/pre-implementation-checklist.md`, and the slice spec
   under `docs/specs/<slice-slug>/specs.md`.
2. If the slice lacks a concrete spec, use `mylite-start-slice` first instead
   of improvising.
3. Confirm affected layers: embedded bootstrap, API, handler/storage engine,
   native storage configuration, metadata, database-directory lifecycle,
   compatibility matrix, wire-protocol integration, build, tests, or docs.
4. Implement through the right layer boundaries. Do not hide product semantics
   behind wrappers that keep durable state outside the MyLite database
   directory.
5. Preserve upstream MariaDB code shape unless a local MyLite module owns the
   change.
6. Add or update tests while implementing. Cover lifecycle, errors, metadata,
   storage invariants, compatibility behavior, write-concurrency assumptions,
   and regression-sensitive behavior relevant to the slice.
7. Update specs, architecture/API docs, and compatibility matrices when the
   implementation changes design or supported behavior.
8. Run relevant builds, tests, and static checks. Fix failures at the root.
9. Self-review the diff for drop-in compatibility, single-directory and native
   storage correctness, embedded lifetime, upstream delta size, binary-size
   impact, license/dependency impact, missing tests, and accidental broad
   changes.

## Done

- The slice works end to end for its stated scope.
- Tests and verification commands pass or any blocker is documented with
  evidence.
- Docs match the implementation.
- Compatibility claims are backed by tests or marked unsupported.
- The diff is focused and ready to commit.
