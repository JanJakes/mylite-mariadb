# CI Docker Build Record Suppression

## Problem

The split WordPress PHPUnit CI path now runs one setup Docker image build and
one runtime Docker image build per PHPUnit shard. CI run `27920287939` on
`3488db7c3` was green, but it produced one Docker build-record artifact for
each `docker/build-push-action@v7` invocation. The run listed `37` extra
`*.dockerbuild` artifacts alongside the MyLite timing artifacts, and an
unfiltered `gh run download 27920287939` failed while trying to extract one of
those build records.

The same run's timing rollup showed:

- `wordpress_docker_build_seconds=822.000` summed across setup and shard jobs;
- `wordpress_artifact_download_seconds=140.000`;
- `wordpress_artifact_extract_seconds=108.000`;
- `wordpress_artifact_extract_runtime_tar_bytes_sum=5718303324`;
- critical shard `non-isolated-xmlrpc`, where a Docker-image step took `71s`;
- estimated WordPress workflow critical path `549s`.

The immediate fix is not to redesign image distribution. This slice removes
unneeded build-record uploads from the existing production timing path so the
artifact set remains focused on MyLite's timing outputs and does not break
normal artifact collection.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- The WordPress CI workflow uses `docker/build-push-action@v7` in three steps:
  `Build WordPress setup Docker image`,
  `Build WordPress runtime Docker image cache`, and
  `Build WordPress shard runtime Docker image`.
- Docker's build-push-action documentation says build record upload is enabled
  by default and can be disabled with the `DOCKER_BUILD_RECORD_UPLOAD`
  environment variable. The same documentation warns that build-record
  artifacts can break broad `actions/download-artifact` collection unless they
  are filtered out.
- `tools/check-ci-production-builds` already owns the workflow guardrails for
  production build types, split WordPress timing phases, and disabled
  heavyweight profiling.

## Design

Set `DOCKER_BUILD_RECORD_UPLOAD: "false"` on every WordPress
`docker/build-push-action@v7` step:

- the setup image build;
- the setup job's runtime cache seed build;
- each shard's runtime image build.

Keep Docker job summaries enabled. The build summary remains useful for
debugging cache behavior, while the downloadable build-record artifacts are not
needed by MyLite's CI timing pipeline.

Extend `tools/check-ci-production-builds` so those three steps must keep the
environment marker. This makes the artifact suppression part of the audited CI
production timing contract.

## Non-Goals

- Changing the Docker image contents, Docker layer cache scopes, or WordPress
  test runtime environment.
- Replacing per-shard Docker image loading with a registry or image artifact.
- Changing WordPress PHPUnit shard filters or test coverage.
- Claiming a throughput win for MyLite engine code.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout,
ownerless-concurrency, or WordPress application behavior changes. This is a CI
artifact hygiene and timing-noise reduction.

## Build And Performance Impact

GitHub Actions should stop uploading the extra Docker build-record artifacts.
The measured Docker build/load work still occurs and remains timed by the
existing `wordpress_docker_build_seconds` rows. Any elapsed-time reduction is
expected to come only from avoiding build-record archive upload and from
preventing artifact download failures during analysis.

## Test Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run focused production CTest coverage for the CI production-build audit.
- Run the production format check.
- Run `git diff --check`.
- Let the next pushed CI run prove that no `*.dockerbuild` artifacts are
  published while WordPress timing artifacts remain available.

## Acceptance Criteria

- Every WordPress Docker build step disables build-record upload.
- The production-build audit fails if any of those markers disappear.
- MyLite timing artifacts remain downloadable without filtering Docker
  build-record artifacts.
- CI still uses Release MyLite PHP builds and a MinSizeRel MariaDB embedded
  archive for WordPress timings.

## Verification Results

Local verification completed:

```text
bash -n tools/check-ci-production-builds
tools/check-ci-production-builds
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
cmake --build --preset format-check-prod
git diff --check
```

The production-build audit passed and reported the guarded workflow path. The
focused production CTest wrapper passed one `tools.ci-production-builds` test
in `3.12 sec`. The production format check and whitespace diff check passed.

The next pushed CI run is the authoritative proof that GitHub stops publishing
Docker build-record artifacts for the three audited Docker build steps.

## Risks And Follow-Up

This removes artifact churn but does not remove the larger fixed per-shard
Docker image cost. Latest timing still points at image setup and artifact
fanout as the structural CI overhead. A later slice can evaluate registry-based
runtime image reuse or a different shard execution model, but that must keep
the production timing environment comparable.
