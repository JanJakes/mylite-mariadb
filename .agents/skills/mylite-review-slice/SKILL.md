---
name: mylite-review-slice
description: Review a completed or near-complete MyLite engineering slice as a release gate for single-file semantics, embedded API behavior, MySQL/MariaDB compatibility, storage-engine routing, tests, docs, binary size, and upstream-fork hygiene. Use only for substantive slice reviews, not simple status checks or small copyedits.
---

# MyLite review slice

Review as a release gate. Find gaps, fix what is safe to fix, and make the
slice line up with the design, tests, and product invariants.

## Trigger boundary

Use this skill when the user asks for a thorough, final, release-gate, or
architecture-focused review of a MyLite slice, implementation diff, or
substantial subsystem. Also use it for project setup or workflow reviews when
the request asks whether the current docs, commit shape, or skills are fit for
future MyLite engineering work.

Do not use this skill for simple git commands, amend/push requests, status
checks, lightweight Q&A, or small documentation copyedits. For ordinary code
reviews that are not MyLite slice release gates, use the normal review
stance.

Do not infer subagent permission from this skill. Use subagents only when the
active session, tool policy, and user request explicitly allow delegation.

## Review checklist

1. Read `README.md`, `AGENTS.md`, `docs/architecture/engineering-standards.md`,
   `docs/architecture/pre-implementation-checklist.md`, the relevant slice
   spec, and the implementation diff. For setup reviews, also read
   `docs/specs/README.md` and the local skill files.
2. Confirm the slice has a concrete spec under `docs/specs/<slice-slug>/` when
   the work is substantial. For setup reviews, confirm each commit remains
   self-contained and does not introduce links to files added only later.
3. Check MariaDB source assumptions against the selected base ref. Use MySQL
   behavior as compatibility evidence for drop-in API or application behavior.
   Treat stale or guessed source claims as review findings.
4. Check single-file semantics. The slice must not introduce persistent sidecar
   files unless the spec explicitly marks them as temporary non-final behavior.
5. Check embedded lifecycle. Opening, closing, repeated initialization, cleanup,
   and handle ownership must be explicit.
6. Check upstream fork hygiene. MariaDB-derived files should have narrow,
   reviewable changes without unrelated formatting churn.
7. Check drop-in compatibility. Supported MySQL/MariaDB API, SQL, storage-engine
   routing, and protocol claims must have tests or explicit unsupported status.
8. Check API boundaries. Public `libmylite` surfaces should not expose
   internal MariaDB handles as the primary abstraction.
9. Check tests for depth and relevance. Add missing tests when the fix is clear;
   otherwise report the gap as blocking.
10. Check license and dependency implications when the slice changes public
    packaging, linking, or imported code.
11. Check docs: slice spec, architecture docs, API docs, compatibility matrix,
    unsupported behavior, and source references.
12. Check skill boundaries and composition. `mylite-start-slice`,
    `mylite-implement-slice`, `mylite-review-slice`, `mylite-upstream-work`,
    `mylite-dont-stop`, and `mylite-work-hard` should have non-overlapping
    trigger boundaries and a clear operating order.
13. Run relevant tests and checks. Fix failures when the fix is clear; otherwise
    report the blocker with the failing command and evidence.
14. Review final diff quality: focused scope, performance awareness, lean
    dependency footprint, binary-size awareness, and readable commit shape.

## Output

- Lead with blocking findings, then fixes made.
- State tests and checks run.
- State remaining risks, if any.
- Do not mark complete until docs, tests, and implementation agree.
