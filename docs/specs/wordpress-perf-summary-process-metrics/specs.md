# WordPress Perf Summary Process Metrics

## Problem

The WordPress mysqli `perf-probe` prints compact
`wordpress_perf_summary_*` lines for raw PHP process startup, MyLite-extension
startup, process connect/close, implicit object-free close, and derived
connect deltas before it runs the in-process PHP probe body. The CI timing
summary only captured the summary lines emitted by the later PHP probe body,
so the most relevant process-isolated PHPUnit startup and close metrics were
present in logs but absent from the Markdown timing table.

## Source Findings

- `tools/wordpress-phpunit-mysqli-mylite` emits process-level
  `wordpress_perf_summary_*` keys from shell before creating the temporary
  `perf_output` capture file used for the PHP probe body.
- The same script reads `wordpress_perf_summary_*` keys from `perf_output`
  after the PHP probe finishes and appends those values to
  `MYLITE_WORDPRESS_TIMING_SUMMARY_PATH`.
- `.github/workflows/ci.yml` publishes that timing summary in the final
  WordPress job step, so metrics omitted from the summary require log scraping
  even though the table is intended to make performance comparisons visible.
- `docs/specs/wordpress-process-close-attribution/specs.md` documents the
  explicit and implicit process connect/close summary keys as CI-friendly
  output.

## Design

Initialize the `perf_summary_metrics` array before the shell-owned process
startup and process connect/close measurements. Append the shell-emitted
summary values to that array immediately after printing them. When the PHP
probe body finishes, keep appending its captured summary values to the same
array and write the combined set to the timing summary.

This keeps stdout compatibility for existing log readers and only changes the
Markdown timing summary content.

## Compatibility Impact

No SQL, PHP API, mysqli API, MyLite runtime, storage, or CI build behavior
changes. The change only carries existing performance metrics into the already
published timing summary.

## Directory And Lifecycle Impact

No durable files, database directories, or lifecycle behavior change. The
probe still reuses the prepared WordPress MyLite test directory and its
temporary perf output file.

## Native Storage Impact

No native MariaDB or InnoDB storage behavior changes.

## Build And Performance Impact

No build-profile changes. The timing summary grows by a bounded set of existing
process metrics: build type, iteration counts, stock PHP process startup,
extension-loaded process startup, extension overhead, explicit and implicit
process connect/close, and derived connect/close deltas.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds` to keep the production timing guards
  intact.
- If the warmed WordPress Docker/build environment is available, run a reduced
  guarded `MYLITE_WORDPRESS_PHASE=perf-probe` and verify the Markdown timing
  summary includes both process-level and in-process `wordpress_perf_summary_*`
  keys.

## Acceptance Criteria

- The timing summary includes process-level WordPress perf summary keys without
  losing the existing in-process SQL/connect summary keys.
- Existing stdout metric names remain unchanged.
- Production build guards and shell syntax checks pass.
