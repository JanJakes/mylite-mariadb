---
name: mylite-dont-stop
description: Persist through substantial MyLite research, implementation, import, or multi-step engineering work when the user asks Codex to keep going despite broad scope. Do not use for simple git commands, status checks, one-off reviews, small documentation edits, or ordinary amend/push requests.
---

# MyLite don't stop

Use this skill when the next instinct is to pause, phase the work, propose a
smaller subset, or ask whether to continue. The default is to continue.

## Trigger boundary

Use this skill only for substantial MyLite work where persistence is the
point: broad implementation, difficult source import, multi-step storage or
runtime work, or requests to keep working across more than one natural stopping
point.

Do not use this skill for simple commands or narrow maintenance work, including
`git status`, routine commits, routine pushes, single-document copy edits, small
documentation corrections, lightweight Q&A, or ordinary review prompts that fit
`mylite-review-slice`. Execute those requests directly.

Do not infer subagent permission from this skill. Use subagents only when the
active session, tool policy, and user request explicitly allow delegation.

## Hard part first

Before acting, identify the hardest part in one sentence. Make one concrete
technical call in two lines. Then do the work.

## Rules

- Produce the artifact, not a proposal about the artifact.
- Attack the central MariaDB fork, drop-in compatibility, embedded lifecycle,
  database-directory lifecycle, native storage configuration, or build problem
  first; let scaffolding follow.
- For new engineering slices, do not skip `docs/specs/<slice-slug>/specs.md`
  unless the task is explicitly small enough not to need a spec.
- Use MariaDB source and official MariaDB documentation for the selected base
  line as the primary authority.
- Use MySQL behavior as compatibility evidence when the task affects drop-in
  API or application behavior.
- Do not split work across sessions unless the user explicitly asks.
- Do not offer a smaller version when the requested version can be done.
- Do not ask preference questions when a reasonable default exists.
- Do not stop after analysis when code, tests, docs, commits, or pushes are
  still needed for the user's request.
- When another MyLite skill owns the phase, follow that skill's preflight,
  spec, implementation, or review gates. Persistence is an overlay, not a
  substitute for the start/implement/review/upstream workflows.

## Guardrail

Ask exactly one focused question only when progress literally requires
information only the user can provide: missing code or files, credentials,
endpoints, or an unspecified hard constraint. State what will be produced when
the answer arrives, then stop.

If no guardrail applies, keep working until the request is implemented,
verified, documented, committed, and pushed when that is part of the active
workflow.
