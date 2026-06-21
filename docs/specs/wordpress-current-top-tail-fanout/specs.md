# WordPress Current Top Tail Fanout

## Problem

The block/token split removed the old block-token critical path, but green CI
run `27913295801` on `e88fecb2` still showed a cluster of similar top shard
times:

- setup total: `67s`;
- critical shard: `non-isolated-block-support-template` at `77s`;
- block support/template PHPUnit shell real: `44.068s`;
- next total tails: `non-isolated-rest-content-post` at `74s`,
  `non-isolated-media-comment` at `71s`, and
  `non-isolated-remaining-platform` at `70s`;
- slowest pure PHPUnit body:
  `phpunit-non-isolated-rest-content-post` at `49.619s` shell real;
- estimated WordPress workflow critical path: `144s`.

Splitting only the current critical shard would immediately move the critical
path to REST content-post or media/comment. This slice fans out the measured
top PHPUnit bodies together while preserving the production-build and
test-only boundaries.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- The workflow keeps broad union regexes for REST content-post, block
  support/template, and media/comment so existing remaining-shard exclusions
  do not change.
- A local direct `test*` method proxy over the pinned WordPress sources found:
  - REST content-post: `53` methods, split into primary content controllers
    (`34`) and revision/autosave history controllers (`19`);
  - block support/template: `187` methods, split into block supports (`93`)
    and block template/binding/token classes (`94`);
  - media/comment/XML-RPC: `742` methods, split into media/attachment (`59`),
    comment (`419`), and XML-RPC (`264`).
- The replacement shard sets exactly cover each retired proxy bucket:
  `overlap=0`, `missing=0`, and `extra=0`.

## Design

Replace three broad visible shards with narrower visible shards:

- `non-isolated-rest-content-post` becomes
  `non-isolated-rest-content-primary` and
  `non-isolated-rest-content-history`;
- `non-isolated-block-support-template` becomes
  `non-isolated-block-supports` and
  `non-isolated-block-template-binding`;
- `non-isolated-media-comment` becomes `non-isolated-media`,
  `non-isolated-comment`, and `non-isolated-xmlrpc`.

Keep the broad REST content-post, block support/template, and media/comment
regexes as documented unions and keep the broader REST controller,
block/token, and media/comment regexes as remaining-shard exclusion
authorities. The split changes only visible matrix jobs, shard case arms, and
the production-build audit.

All replacement shards keep the same production test-only settings as the
retired combined shards: Release MyLite PHP artifacts, MinSizeRel MariaDB
embedded archive, runtime manifest verification, restored prepared baseline
database, keepalive enabled, parent install skip enabled, child install skip
disabled, default PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting individual WordPress classes by method name.
- Changing the broad REST, block/token, media/comment, or remaining-shard
  exclusion boundaries.
- Optimizing Docker image setup, artifact transfer, native open/close, or
  ownerless engine execution in this slice.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, storage, directory, native recovery, or
WordPress application behavior changes. CI still runs the same WordPress test
surface with the same production MyLite and MariaDB artifacts.

## Directory And Lifecycle Impact

No durable layout changes. Each shard continues to restore the same prepared
WordPress MyLite database baseline into the same external temporary parent
directory before its test-only phase.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, or ownerless concurrency
behavior changes.

## Build, Size, License, And Dependency Impact

No compiled-code, binary-size, license, or dependency changes. CI gains four
additional WordPress PHPUnit shard jobs: one from REST content-post fanout, one
from block support/template fanout, and two from media/comment fanout. The
expected benefit is a shorter WordPress critical path and clearer top-tail
timing attribution, at the cost of more parallel artifact download/extract and
Docker image reuse paths.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition proof over the pinned WordPress direct
  `test*` method proxy for the full non-isolated shard set.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the new top-tail fanout
  shards.

## Acceptance Criteria

- CI contains distinct REST content primary and history shard labels, and the
  old visible `phpunit-non-isolated-rest-content-post` label and case arm are
  rejected by the production-build audit.
- CI contains distinct block supports and block template/binding shard labels,
  and the old visible `phpunit-non-isolated-block-support-template` label and
  case arm are rejected by the production-build audit.
- CI contains distinct media, comment, and XML-RPC shard labels, and the old
  visible `phpunit-non-isolated-media-comment` label and case arm are rejected
  by the production-build audit.
- The broad REST content-post, block support/template, and media/comment
  regexes remain present as union authorities.
- Focused PCRE samples show pinned WordPress direct `test*` proxy names match
  exactly one non-isolated shard or none when intentionally excluded.
- Each retired proxy bucket is exactly covered by its replacement shard set.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `e88fecb2` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress direct `test*`
  method proxy and reported `overlap_count=0`,
  `rest_content_post_union_overlap=0 missing=0 extra=0 old=53 split=53`,
  `block_support_template_union_overlap=0 missing=0 extra=0 old=187 split=187`,
  and `media_comment_union_overlap=0 missing=0 extra=0 old=742 split=742`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for the new top-tail fanout
shards.

## Risks

- Static direct `test*` method counts do not expand data-provider cases. CI
  timing remains the authority for the actual wall-clock split.
- Adding four matrix jobs increases total runner work. The current timing data
  shows several top tails are close together, so splitting them together is
  more useful than splitting only the current critical shard.
- Fixed artifact and Docker-image overhead remains visible. Removing it would
  require a larger CI architecture change that could alter the timing
  environment; this slice keeps the test runtime shape constant.
