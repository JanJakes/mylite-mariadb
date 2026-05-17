# MTR Probe Harness

## Goal

Add a `probe` command to `tools/mylite-mtr-harness` so candidate MariaDB MTR
tests can be evaluated repeatably without stopping at the first failure. A
candidate only counts as usable coverage when MTR reports an actual `[ pass ]`
line; skipped tests and ordinary failures remain non-coverage.

## Non-Goals

- Adding new curated default MTR tests.
- Classifying every upstream MTR failure mode.
- Replacing the strict `run` command used for accepted coverage.
- Adding a quarantine, allowlist, or automatic result normalization policy.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- The existing `run` command correctly fails when a selected test does not
  produce a pass line, but it stops on the first failed/skipped candidate.
- Candidate discovery has repeatedly needed shell loops that run one exact MTR
  test at a time and preserve the distinction between real passes, skips, and
  failures.
- MTR can exit successfully for a skipped selected test; the harness must keep
  using pass-line assertion rather than process exit status alone.
- Failed upstream MTR candidates can generate `*.reject` files under
  `mariadb/mysql-test/main`. Probe is a discovery workflow, so it should not
  leave newly generated rejects in the source tree.

## Compatibility Impact

No compatibility status changes. This is harness workflow support for future
compatibility slices.

## Design

- Add `tools/mylite-mtr-harness probe suite.test...`.
- Reuse the same MTR build preparation, exact suite/case anchoring, embedded
  options, default storage engine, and pass-line assertion as `run`.
- Continue after failed or skipped candidates, print `PASS <suite.test>` or
  `FAIL <suite.test>`, and return nonzero if any candidate does not report an
  MTR pass.
- Snapshot existing `mariadb/mysql-test/**/*.reject` files before probing and
  remove only rejects created during probe execution.
- Keep `run` strict: it still stops at the first failed or skipped selected
  test and preserves failure artifacts for debugging.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. Probe runs use
the existing `build/mariadb-mtr-smoke/mysql-test/var` MTR work area.

## Embedded Lifecycle And API

No `libmylite` API change. This only changes the MTR harness command surface.

## Build, Size, And Dependencies

No dependency or production binary-size change. The command is implemented in
the existing Bash harness.

## Test Plan

- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness probe main.prepare`
- `! tools/mylite-mtr-harness probe main.ansi` and verify the generated
  `mariadb/mysql-test/main/ansi.reject` file is removed.
- `! tools/mylite-mtr-harness run main.ansi` and verify strict `run` leaves
  `mariadb/mysql-test/main/ansi.reject` for debugging.
- `tools/mylite-mtr-harness run main.prepare`
- `tools/mylite-mtr-harness run`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `probe` requires at least one `suite.test` argument.
- `probe` continues after failed or skipped candidates.
- `probe` returns success only when every selected candidate reports an MTR
  pass.
- `probe` removes newly generated `.reject` files without deleting reject files
  that existed before the probe started.
- `run` keeps its strict accepted-coverage behavior.

## Risks And Open Questions

- Probe summaries intentionally show only the tail of the MTR output. Deeper
  failure analysis still belongs in the full MTR log under the build tree.
- Cleaning probe-generated rejects trades source-tree hygiene for less
  convenient immediate diff inspection. Full failure logs remain under the MTR
  build tree for deeper analysis.
- A future CI dashboard may need machine-readable probe output, but plain text
  is sufficient for local candidate discovery.
