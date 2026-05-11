---
name: mylite-start-slice
description: Research and design a bounded MyLite engineering slice before implementation, producing docs/specs/<slice>/specs.md with MariaDB source references, architecture decisions, test strategy, and acceptance criteria. Use only for substantial start/design work, not simple docs edits, reviews, commits, pushes, or status commands.
---

# MyLite start slice

Use this skill before implementation. Produce the research, design, docs, and
test plan that make a MyLite engineering slice implementable and reviewable.

## Trigger boundary

Use this skill when the user asks to start, research, design, specify, or plan a
substantial MyLite slice such as embedded bootstrap, `libmylite` API,
storage-engine discovery, single-file catalog, build trimming, crash recovery,
DDL metadata routing, or upstream import strategy.

Do not use this skill for simple commands, routine git operations, small docs
corrections, review-only prompts, or questions that can be answered without
opening a new slice plan.

Do not infer subagent permission from this skill. Use subagents only when the
active session, tool policy, and user request explicitly allow delegation.

## Source discipline

Use MariaDB source and official MariaDB documentation for the selected base line
as the primary authority. Record exact refs, paths, and relevant functions.
MySQL behavior may be useful historical context, but it is not the normative
compatibility target for MyLite.

## Workflow

1. Read `README.md`, `AGENTS.md`, `docs/architecture/engineering-standards.md`,
   `docs/architecture/pre-implementation-checklist.md`, and the architecture/API
   docs relevant to the slice.
2. Choose a lower-case hyphenated slice slug and create
   `docs/specs/<slice-slug>/`.
3. Identify the MariaDB base version and source refs used for the design.
4. Inspect the relevant MariaDB source paths directly. Do not rely only on
   assumptions from docs or names.
5. Define scope and non-goals. Keep the slice bounded enough to implement and
   review.
6. Write `docs/specs/<slice-slug>/specs.md` with:
   - problem statement,
   - source findings with refs,
   - proposed design,
   - affected MariaDB subsystems,
   - DDL metadata routing impact when the slice touches table definitions,
   - single-file and embedded-lifecycle implications,
   - public API or file-format impact,
   - binary-size impact when relevant,
   - license, trademark, or dependency impact when relevant,
   - test and verification plan,
   - acceptance criteria,
   - risks and unresolved questions.
7. Update related architecture docs when the slice changes an existing design
   decision.
8. Do not begin substantial implementation until the spec is concrete enough to
   verify the result.

## Done

- Slice docs live in `docs/specs/<slice-slug>/specs.md`.
- The design is grounded in MariaDB source and official documentation.
- Single-file, embedded-runtime, upstream-delta, and binary-size implications
  are explicit where relevant.
- Tests and acceptance criteria are clear.
- Remaining risks are documented rather than hidden.
