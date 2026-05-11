# Engineering slice specifications

Substantial MyLite work starts with an engineering slice specification under
this directory. Start with the
[pre-implementation checklist](../architecture/pre-implementation-checklist.md),
then write the slice spec at:

Each slice should use a lower-case, hyphenated directory name:

```text
docs/specs/<slice-slug>/specs.md
```

Useful slice examples:

- `upstream-11-8-import`
- `embedded-bootstrap`
- `libmylite-open-close`
- `storage-engine-skeleton`
- `mylite-engine-discovery`
- `application-engine-routing`
- `ddl-metadata-routing`
- `single-file-catalog`
- `storage-engine-compatibility-matrix`
- `compatibility-test-harness`
- `build-profile-minsize`
- `unsupported-server-surface`

Each `specs.md` should capture:

- the problem being solved,
- scope and non-goals,
- MariaDB base version and source references,
- relevant official MariaDB documentation,
- MySQL/MariaDB compatibility impact,
- affected runtime, storage, build, or API layers,
- proposed design,
- single-file and embedded-lifecycle implications,
- storage-engine routing impact when relevant,
- wire-protocol or integration-package impact when relevant,
- binary-size implications when relevant,
- test and verification plan,
- acceptance criteria,
- risks and unresolved questions.

Specs should be specific enough that implementation and review can verify the
work without rediscovering the design from scratch.

Update architecture docs and compatibility matrices when a slice changes a
supported behavior claim, storage-engine routing, protocol behavior,
unsupported surface, or file-lifecycle guarantee.

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

State how the slice affects the primary `.mylite` file, companion files,
temporary files, recovery, cleanup, and unsupported persistent sidecars.

## Embedded Lifecycle And API

Cover open/close, repeated initialization, handle ownership, diagnostics, and
public API behavior when relevant.

## Build, Size, And Dependencies

Record build-profile, binary-size, dependency, license, or fork-maintenance
impact when relevant.

## Test Plan

List unit, integration, compatibility, recovery, concurrency, sidecar,
static-analysis, and size checks needed for acceptance.

## Acceptance Criteria

List the concrete conditions that make the slice done.

## Risks And Open Questions

Record unresolved technical risks instead of hiding them in prose.
```
