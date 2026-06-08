# Ownerless Page Log Initialized Append

## Problem

Production ownerless autocommit insert probes now show external refresh counters
at zero, leaving page-version publication as the next visible cost. A reduced
400-row production probe reported:

- `page_publish_published=3203`;
- `page_log_append_calls=3205`;
- `page_log_append_total_ms=168.867`;
- `page_log_append_header_ms=9.620`;
- `page_log_append_body_ms=145.412`;
- ownerless autocommit insert rate around `200.86 ops/s`.

The page-log header is initialized at ownerless runtime open via
`mylite_ownerless_page_log_initialize_at()`, but the hot append path still
validates the header before every page record append.

## Design

Add a narrow `mylite_ownerless_page_log_append_initialized_at()` API that:

- keeps the existing append lock and body append logic;
- skips repeated header validation;
- assumes the caller already initialized the page log at the same offset;
- still rejects uninitialized or truncated files through the existing body-size
  checks.

The general `mylite_ownerless_page_log_append_at()` API remains conservative and
continues to validate or create the header. The ownerless InnoDB page publish
hook is the intended initialized caller because runtime open already validates
the page log before hooks are installed.

## Compatibility Impact

No SQL, C API, PHP API, storage-format, or page-log format changes. The new
helper is internal first-party API used by the ownerless runtime append path.

## Test Plan

- Add primitive page-log coverage proving initialized append fails before
  initialization, succeeds after `initialize_at()`, returns the expected record
  offsets, and remains readable through existing page-log read APIs.
- Run production primitive coverage.
- Run ownerless focused selectors and reduced production performance probes.
- Run `format-check-prod` and `git diff --check`.

## Risks And Follow-Up

- This only removes repeated header validation from initialized runtime appends.
  Payload writes, checksums, record-header writes, and page-index publication
  remain the dominant page-publication costs.
- Larger reductions require a separate design for page-publication write
  amplification, peer-open visibility, or native dirty-page handoff.
