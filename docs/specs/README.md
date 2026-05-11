# Engineering Slice Specs

Use a spec when a roadmap slice needs source-linked design before code. Start
with the [pre-implementation checklist](../architecture/pre-implementation-checklist.md),
then write the slice spec at:

```text
docs/specs/<slice-slug>/specs.md
```

Keep specs short enough to review and specific enough to implement without
rediscovering the design.

## Template

```markdown
# <Slice Name>

## Goal

State the behavior this slice will add or change.

## Non-Goals

List related behavior that this slice deliberately will not solve.

## Source Findings

- MariaDB base: `<tag>` / `<commit>`
- Source refs: `<path>:<function-or-section>`
- Official docs: `<links>`

Summarize the relevant upstream behavior and any uncertainty.

## Compatibility Impact

Identify affected MySQL/MariaDB API, SQL, storage-engine, metadata, or protocol
behavior. State whether `docs/COMPATIBILITY.md` needs to change.

## Design

Describe the implementation boundary, affected MyLite modules, and important
MariaDB integration points.

## File Lifecycle

State how the slice affects the MyLite database directory, native engine files,
companion files, temporary files, recovery, cleanup, and unsupported persistent
state outside the directory.

## Embedded Lifecycle And API

Cover open/close, repeated initialization, handle ownership, diagnostics, and
public API behavior when relevant.

## Build, Size, And Dependencies

Record build-profile, binary-size, dependency, license, or fork-maintenance
impact when relevant.

## Test Plan

List unit, integration, compatibility, recovery, concurrency,
directory-boundary, static-analysis, and size checks needed for acceptance.

## Acceptance Criteria

List the concrete conditions that make the slice done.

## Risks And Open Questions

Record unresolved technical risks instead of hiding them in prose.
```
