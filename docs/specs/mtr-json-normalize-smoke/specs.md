# MTR JSON Normalize Smoke

## Goal

Promote MariaDB's `main.json_normalize` MTR case into the curated embedded MTR
smoke list with the same engine-output normalization used by other accepted
MTR smoke tests.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/json_normalize.test` covers `JSON_NORMALIZE()` over
  JSON object/array formatting, key ordering, numeric normalization, invalid
  JSON, view projection, and latin1-to-utf8mb4 conversion behavior.
- The MTR smoke profile uses `--default-storage-engine=Aria` and disables
  native MyISAM, while upstream expected output records default `ENGINE=MyISAM`
  for the initial `SHOW CREATE TABLE`.
- The accepted local normalization is the same narrow pattern used elsewhere:
  replace `ENGINE=Aria` with `ENGINE=MyISAM` and remove ` PAGE_CHECKSUM=1`
  around the affected `SHOW CREATE TABLE`.

## Scope

- Add `main.json_normalize` to `tools/mylite-mtr-harness`.
- Add a narrow result normalizer before the one affected `SHOW CREATE TABLE`.
- Update MTR smoke documentation and compatibility summaries.

## Non-Goals

- Enabling unsupported JSON table/schema surfaces.
- Changing MyLite runtime JSON behavior.
- Changing the MTR smoke profile's default storage engine.
- Size-profile reduction work.

## Compatibility Impact

The curated MTR smoke runner now covers JSON normalization behavior in addition
to JSON equality behavior. This remains MariaDB embedded MTR smoke coverage,
not a full MyLite storage-routing claim.

## Design

No production change is required. The upstream test body is preserved; only
default-engine noise is normalized in the result stream.

## File Lifecycle

No MyLite file-format or companion-file change is introduced.

## Embedded Lifecycle And API

No `libmylite` API change is required.

## Storage-Engine Routing

The test runs in the MTR smoke profile with default Aria tables. It does not
claim MyLite storage-engine routing behavior.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Run `tools/mylite-mtr-harness run main.json_normalize`.
- Run the full curated MTR smoke list.
- Verify the documented curated list matches `tools/mylite-mtr-harness list`.
- Run shell syntax checks, reject-file cleanup checks, and `git diff --check`.

## Acceptance Criteria

- `main.json_normalize` reports MTR `[ pass ]`.
- The full curated MTR smoke list remains green.
- The normalizer only changes the known default-engine result drift.
- MTR smoke docs and compatibility summaries mention JSON normalization.

## Risks And Open Questions

- Broader JSON runtime coverage, especially unsupported table/schema surfaces,
  remains intentionally separate from this smoke promotion.
