# Engineering slice specifications

Substantial MyLite work starts with an engineering slice specification under
this directory.

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
- `ddl-metadata-routing`
- `single-file-catalog`
- `build-profile-minsize`
- `unsupported-server-surface`

Each `specs.md` should capture:

- the problem being solved,
- scope and non-goals,
- MariaDB base version and source references,
- relevant official MariaDB documentation,
- affected runtime, storage, build, or API layers,
- proposed design,
- single-file and embedded-lifecycle implications,
- binary-size implications when relevant,
- test and verification plan,
- acceptance criteria,
- risks and unresolved questions.

Specs should be specific enough that implementation and review can verify the
work without rediscovering the design from scratch.

Use [../architecture/pre-implementation-checklist.md](../architecture/pre-implementation-checklist.md)
to choose and scope the first slices. Use [../ROADMAP.md](../ROADMAP.md) for
the current implementation order and progress status.
