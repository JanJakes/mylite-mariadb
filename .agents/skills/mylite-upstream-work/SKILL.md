---
name: mylite-upstream-work
description: Manage MyLite upstream MariaDB import, rebase, or patch-stack work with clean fork discipline, recorded refs, separated upstream and local changes, compatibility-impact review, and verification. Use for upstream source import/rebase/update work, not ordinary docs edits or isolated first-party changes.
---

# MyLite upstream work

Use this skill for importing MariaDB source, rebasing onto a new MariaDB ref, or
changing the MyLite patch stack over upstream MariaDB.

## Trigger boundary

Use this skill when the user asks to import MariaDB, update the upstream base,
rebase MyLite, refresh patches, compare against upstream, or make changes
inside upstream-derived source where fork hygiene is central.

Do not use this skill for standalone first-party files, small docs changes,
ordinary feature implementation, or status commands.

Do not infer subagent permission from this skill. Use subagents only when the
active session, tool policy, and user request explicitly allow delegation.

## Preflight

- Read `git status` and the current branch before editing.
- Identify the current MyLite base and target MariaDB ref.
- Keep unrelated user changes intact.
- Decide whether the work is an upstream import/rebase commit, a MyLite patch
  commit, or both. Keep those commits separate.

## Workflow

1. Read `README.md`, `AGENTS.md`, `docs/architecture/engineering-standards.md`,
   `docs/architecture/pre-implementation-checklist.md`, and any existing
   upstream/import docs.
2. Fetch or inspect the target MariaDB source ref.
3. Record the exact upstream ref, tag, branch, and source repository URL in the
   relevant docs or commit body.
4. Keep upstream source import mechanical. Avoid local edits in the same commit.
5. Apply MyLite patches as narrow follow-up changes.
6. Preserve upstream formatting and file organization in MariaDB-derived files.
7. Update docs and compatibility matrices when an upstream change affects
   embedded bootstrap, handler APIs, storage discovery or routing, SQL/API
   behavior, build options, or licensing assumptions.
8. Run the relevant configure/build/tests for the changed base where possible.
9. Compare the final diff against upstream and remove accidental churn.

## Done

- Upstream ref is recorded.
- Mechanical upstream changes are separate from MyLite patches.
- MyLite deltas are focused and documented where needed.
- Relevant build/test checks were run, or blockers are documented.
