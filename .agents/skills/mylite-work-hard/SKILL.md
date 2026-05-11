---
name: mylite-work-hard
description: Continuously advance MyLite by selecting and completing batches of engineering slices from the roadmap, using the start/implement/review loop and committing completed work. Use only when the user explicitly asks for ongoing or batch progress, not simple commands or one-off edits.
---

# MyLite work hard

Move MyLite forward continuously. Pick a coherent batch, finish it end to
end, commit it, then continue with the next useful batch when the user asked for
ongoing progress.

## Trigger boundary

Use this skill only when the user explicitly asks for ongoing or batch MyLite
progress: "work hard", "next batch", "keep going", "continue the roadmap",
"pick the next slices", or similar wording that clearly means more than one
isolated change.

Do not use this skill for simple commands, routine commits, amend/push
requests, one-slice implementation, review-only prompts, small docs edits, or
status checks.

Do not infer subagent permission from this skill or from "work hard" wording.
Use subagents only when the active session, tool policy, and user request
explicitly allow delegation.

## Preflight

- Read the current branch and `git status`.
- Do not overwrite unrelated user changes.
- Read `README.md`, `AGENTS.md`, and
  `docs/architecture/engineering-standards.md`.
- Check `docs/architecture/pre-implementation-checklist.md` before picking
  early implementation work.
- Identify the highest-value roadmap area from current docs and code.

## Operating loop

1. Pick a small coherent batch of engineering slices. Prefer work where the
   slices reinforce one another, such as embedded bootstrap plus open/close
   lifecycle tests, storage-engine routing plus catalog docs, or compatibility
   harness work plus coverage-matrix updates.
2. For each substantial slice:
   - use `mylite-start-slice` to research and specify it;
   - use `mylite-implement-slice` to implement and verify it;
   - use `mylite-review-slice` to close design, test, and documentation
     gaps.
3. Use `mylite-upstream-work` for MariaDB import, rebase, or patch-stack
   changes.
4. Use `mylite-dont-stop` only as a persistence overlay when the user asked for
   sustained work; it does not replace slice specs, implementation checks, or
   review gates.
5. Run relevant tests and checks for the batch.
6. Commit completed work as atomic, readable commits. Push only when the user
   asked for pushing or the active workflow already requires it.
7. If the user asked for continued progress beyond one batch, immediately pick
   the next batch and continue.

## Persistence rule

Do not stop because the work is broad, repetitive, or difficult. Stop only for a
real blocker that cannot be solved locally: missing credentials, unavailable
network or runtime infrastructure, an unclear user constraint that cannot be
defaulted, or a failing external dependency that cannot be worked around
honestly.
