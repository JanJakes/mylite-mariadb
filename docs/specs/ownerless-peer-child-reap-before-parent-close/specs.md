# Ownerless Peer Child Reap Before Parent Close

## Problem Statement

The ownerless SQL weighted shard timed out in CI on
`test_ownerless_broader_ddl_refreshes_peer_dictionary`. The timeout dump showed
the parent test-case process sleeping while its DDL child was already exited but
not reaped. After a narrow fix for that case, the same shard reproduced locally
at `test_ownerless_mixed_direction_index_ddl_refreshes_peer_dictionary` with the
same parent-plus-zombie-child shape.

The repeated evidence points at staged peer test harness ordering rather than a
single DDL SQL regression: the parent process can enter `mylite_close()` while a
forked peer child has already exited but still has a zombie PID owned by the
parent.

## Source Findings

- The staged peer tests fork one child, coordinate final peer-visible SQL state
  through pipes, then perform parent-side assertions.
- Many tests then closed the parent MyLite handle before reaping the child.
- A zombie PID remains visible to process-liveness checks until the parent calls
  `waitpid()`. Ownerless close-time no-live cleanup is intentionally
  conservative around live peers, so the test harness should not leave already
  exited children visible during parent close.

## Design

Normalize staged peer test tails that matched this shape:

1. close the final parent-side coordination pipes;
2. reap the peer child with `wait_for_child()`;
3. close the parent MyLite handle;
4. run the existing reopen, file-lifecycle, and forced rebuild checks.

This preserves the SQL and metadata assertions while removing the
parent-close-with-zombie-child state from the harness.

## Compatibility Impact

No SQL, C API, storage-format, or production ownerless behavior changes. This is
test-harness lifecycle hardening for existing ownerless DDL, view, trigger,
foreign-key, and index peer-refresh coverage.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` in `php-embedded-prod`.
- Run focused selectors for the two observed timeout points:
  `ddl-broader` and `mixed-direction-index-ddl`.
- Run the weighted shard that timed out in CI:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.12$' --output-on-failure`.
- Run format and whitespace checks.
- Confirm no ownerless test processes or `/tmp/mylite-ownerless-*` directories
  remain.

## Acceptance Criteria

- No staged peer tail in `ownerless_cross_process_sql_test.c` keeps the exact
  old `mylite_close()`-before-pipe-close-and-`wait_for_child()` ordering.
- The focused selectors and weighted shard pass after reordering.
- No production code changes are needed for this slice.

## Risks And Follow-Up

- This does not prove every possible ownerless CI timeout is fixed. It removes
  the parent-close-with-zombie-child state from the staged peer tests where it
  was observed. Later timeouts in different phases should be debugged from their
  process dump rather than assumed to be this lifecycle issue.
