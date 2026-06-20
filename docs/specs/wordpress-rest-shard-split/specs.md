# WordPress REST Shard Split

## Problem

The production WordPress PHPUnit matrix now separates build/setup from
test-only shards and runs the shards in parallel. The green CI run for
`84a744f1` showed the longest remaining test-only gate was
`phpunit-non-isolated-rest`:

- `phpunit-non-isolated-rest`: `178.539s` shell real,
- `phpunit-non-isolated-content-user`: `145.720s`,
- `phpunit-non-isolated-remaining`: `116.857s`,
- `phpunit-non-isolated-query-theme-block-token`: `99.539s`,
- `phpunit-db`: `7.880s`,
- process-isolated shards combined: `59.682s`.

The same run's mysqli performance probe reported production `Release` MyLite
artifacts, PHP process startup at `25.539ms`, process plus connect/close at
`147.071ms`, in-process connect/close at `118.003ms`, active-runtime reconnect
at `2.009ms`, and `SELECT 1` at `1549.39 ops/s`. The slow PHPUnit wall-clock
bucket is therefore the breadth of the REST test shard, not a branch-local
ordinary mysqli startup regression.

## Source Findings

- `.github/workflows/ci.yml` derives non-isolated filters from
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER`.
- The REST shard currently uses a positive lookahead for
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REST_RE`, defined as
  `Tests_REST|WP_Test_REST|WP_REST`.
- The final rollup already aggregates labels matching
  `phpunit-non-isolated-*`, so additional REST labels remain in the existing
  non-isolated totals without tool changes.
- A local scan of the pinned WordPress test classes found the proposed split
  is a partition under the current base non-isolated filter:
  `45` REST classes contain `Controller`, `9` REST classes do not, and no
  class matched more than one proposed non-isolated shard.

## Design

Replace the single REST matrix entry with two entries:

- `phpunit-non-isolated-rest-controller` uses the current REST positive
  lookahead plus `(?=.*Controller)`.
- `phpunit-non-isolated-rest-other` uses the current REST positive lookahead
  plus `(?!.*Controller)`.

Both entries keep the same production-build guards, restored baseline database,
`MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
`MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`, disabled mysqli profiling, and
disabled default PHPUnit logging as the previous REST shard.

The broad `non-isolated-remaining` filter continues to exclude the full REST
regex, so the split changes only the REST partition boundary.

## Compatibility Impact

No SQL behavior, public API behavior, database-directory lifecycle, native
storage behavior, or WordPress compatibility target changes. CI still runs the
same WordPress tests with the same production MyLite and MariaDB artifacts.

## Directory And Lifecycle Impact

No product directory layout changes. Each CI shard still restores its own
transient MyLite WordPress database from the uploaded baseline under `/tmp`.

## Build And Performance Impact

No compiled code changes. The WordPress matrix gains one more shard job, which
adds one extra artifact download/extract and Docker image build in a separate
runner. The intended wall-clock improvement is to replace one roughly
three-minute REST gate with two smaller visible gates while preserving the
aggregate non-isolated timing rows.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE sample against pinned WordPress class names and prove
  the two REST filters, query/theme/block/token filter, content/user filter,
  and remaining filter are mutually exclusive under the base non-isolated
  filter.
- Run `ctest --preset prod -R '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$' --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI collect the first production timings for the two REST subshards.

## Acceptance Criteria

- CI has distinct `phpunit-non-isolated-rest-controller` and
  `phpunit-non-isolated-rest-other` timing labels.
- The production-build audit fails if either REST subshard or its filter is
  removed.
- The split keeps production `Release` and MariaDB embedded `MinSizeRel`
  guards before test-only timings.
- Existing timing rollups still include both REST subshards in the
  non-isolated aggregate.

## Risks And Follow-Up

This is a CI wall-clock and observability optimization, not an engine
throughput improvement. If the first production run shows one REST subshard
still dominates, the next bounded follow-up is a second split using the
reported labels rather than changing MyLite runtime code without evidence.
