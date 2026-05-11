#!/usr/bin/env bash
set -euo pipefail

main() {
  if [[ "${1:-}" == "--inside-container" ]]; then
    run_inside_container
    return
  fi

  local repo_root
  repo_root="$(git rev-parse --show-toplevel)"

  local image
  image="${MYLITE_MARIADB_MINSIZE_IMAGE:-mylite-mariadb-minsize:ubuntu-24.04}"

  docker build \
    -t "${image}" \
    -f "${repo_root}/tools/docker/mariadb-minsize/Dockerfile" \
    "${repo_root}/tools/docker/mariadb-minsize"

  docker run --rm \
    --user "$(id -u):$(id -g)" \
    --volume "${repo_root}:/work" \
    --workdir /work \
    --env "MYLITE_MARIADB_BUILD_DIR=${MYLITE_MARIADB_BUILD_DIR:-build/mariadb-minsize}" \
    --env "MYLITE_BUILD_JOBS=${MYLITE_BUILD_JOBS:-}" \
    "${image}" \
    /work/tools/run-storage-engine-smoke.sh --inside-container
}

run_inside_container() {
  /work/tools/build-mariadb-minsize.sh --inside-container

  local build_dir="${MYLITE_MARIADB_BUILD_DIR:-build/mariadb-minsize}"
  local jobs="${MYLITE_BUILD_JOBS:-}"
  if [[ -z "${jobs}" ]]; then
    jobs="$(nproc)"
  fi

  cmake --build "${build_dir}" \
    --target mylite-storage-engine-smoke \
    --parallel "${jobs}"

  local abs_build_dir
  abs_build_dir="$(cd "${build_dir}" && pwd)"

  local runtime_dir="${abs_build_dir}/mylite-storage-engine"
  local report="${abs_build_dir}/mylite-storage-engine-report.txt"

  local smoke="${abs_build_dir}/mylite/mylite-storage-engine-smoke"
  local status=0

  run_smoke_phase \
    "${smoke}" \
    "${abs_build_dir}/sql/share" \
    "${runtime_dir}" \
    "${report}" \
    "${abs_build_dir}/mylite-storage-engine-output.log" \
    "" \
    "none" || status=1

  local catalog_root="${abs_build_dir}/mylite-catalog-persistence"
  local catalog_file="${catalog_root}/catalog.mylite"
  rm -rf "${catalog_root}"
  mkdir -p "${catalog_root}"

  run_smoke_phase \
    "${smoke}" \
    "${abs_build_dir}/sql/share" \
    "${catalog_root}/write" \
    "${abs_build_dir}/mylite-catalog-write-report.txt" \
    "${abs_build_dir}/mylite-catalog-write-output.log" \
    "${catalog_file}" \
    "write" || status=1

  run_smoke_phase \
    "${smoke}" \
    "${abs_build_dir}/sql/share" \
    "${catalog_root}/read" \
    "${abs_build_dir}/mylite-catalog-read-report.txt" \
    "${abs_build_dir}/mylite-catalog-read-output.log" \
    "${catalog_file}" \
    "read" || status=1

  local recovery_root="${abs_build_dir}/mylite-catalog-recovery"
  local recovery_file="${recovery_root}/catalog.mylite"
  rm -rf "${recovery_root}"
  mkdir -p "${recovery_root}"

  run_smoke_phase \
    "${smoke}" \
    "${abs_build_dir}/sql/share" \
    "${recovery_root}/base" \
    "${abs_build_dir}/mylite-catalog-recovery-base-report.txt" \
    "${abs_build_dir}/mylite-catalog-recovery-base-output.log" \
    "${recovery_file}" \
    "recovery-base" || status=1

  local recovery_payload_offset
  recovery_payload_offset="$(stat -c %s "${recovery_file}")"

  run_smoke_phase \
    "${smoke}" \
    "${abs_build_dir}/sql/share" \
    "${recovery_root}/latest" \
    "${abs_build_dir}/mylite-catalog-recovery-latest-report.txt" \
    "${abs_build_dir}/mylite-catalog-recovery-latest-output.log" \
    "${recovery_file}" \
    "recovery-latest" || status=1

  corrupt_latest_catalog_payload "${recovery_file}" "${recovery_payload_offset}" || status=1

  run_smoke_phase \
    "${smoke}" \
    "${abs_build_dir}/sql/share" \
    "${recovery_root}/read" \
    "${abs_build_dir}/mylite-catalog-recovery-read-report.txt" \
    "${abs_build_dir}/mylite-catalog-recovery-read-output.log" \
    "${recovery_file}" \
    "recovery-read" || status=1

  printf "Storage engine smoke report: %s\n" "${report}"
  printf "Catalog write smoke report: %s\n" "${abs_build_dir}/mylite-catalog-write-report.txt"
  printf "Catalog read smoke report: %s\n" "${abs_build_dir}/mylite-catalog-read-report.txt"
  printf "Catalog recovery base smoke report: %s\n" "${abs_build_dir}/mylite-catalog-recovery-base-report.txt"
  printf "Catalog recovery latest smoke report: %s\n" "${abs_build_dir}/mylite-catalog-recovery-latest-report.txt"
  printf "Catalog recovery read smoke report: %s\n" "${abs_build_dir}/mylite-catalog-recovery-read-report.txt"
  return "${status}"
}

run_smoke_phase() {
  local smoke="$1"
  local lc_messages_dir="$2"
  local runtime_dir="$3"
  local report="$4"
  local smoke_log="$5"
  local catalog_file="$6"
  local persistence_phase="$7"

  local datadir="${runtime_dir}/datadir"
  local tmpdir="${runtime_dir}/tmp"

  rm -rf "${runtime_dir}"
  mkdir -p "${datadir}/mylite" "${tmpdir}"
  rm -f "${smoke_log}" "${report}"

  local args=(
    "--datadir=${datadir}"
    "--tmpdir=${tmpdir}"
    "--lc-messages-dir=${lc_messages_dir}"
    "--runtime-dir=${runtime_dir}"
    "--persistence-phase=${persistence_phase}"
    "--report=${report}"
  )
  if [[ -n "${catalog_file}" ]]; then
    args+=("--catalog-file=${catalog_file}")
  fi

  local status=0
  "${smoke}" "${args[@]}" > "${smoke_log}" 2>&1 || status=$?

  append_observed_files "${runtime_dir}" "${report}" "${smoke_log}"
  append_catalog_files "${catalog_file}" "${report}"
  if [[ -n "${catalog_file}" && -e "${catalog_file}.tmp" ]]; then
    status=1
  fi
  if has_frm_artifacts "${runtime_dir}"; then
    status=1
  fi
  return "${status}"
}

corrupt_latest_catalog_payload() {
  local catalog_file="$1"
  local payload_offset="$2"

  if [[ ! -f "${catalog_file}" ]]; then
    printf "Catalog recovery file does not exist: %s\n" "${catalog_file}" >&2
    return 1
  fi
  if [[ -z "${payload_offset}" || "${payload_offset}" -lt 8192 ]]; then
    printf "Invalid catalog payload offset: %s\n" "${payload_offset}" >&2
    return 1
  fi

  printf '\0' | dd of="${catalog_file}" bs=1 seek="${payload_offset}" count=1 conv=notrunc status=none
}

append_observed_files() {
  local runtime_dir="$1"
  local report="$2"
  local smoke_log="$3"

  {
    printf "\n## Smoke Process Output\n\n"
    if [[ -s "${smoke_log}" ]]; then
      cat "${smoke_log}"
    else
      printf "none\n"
    fi

    printf "\n## Observed Runtime Files\n\n"
    if find "${runtime_dir}" -mindepth 1 -type f | grep -q .; then
      find "${runtime_dir}" -mindepth 1 -type f -printf "%P\t%s bytes\n" \
        | sort
    else
      printf "none\n"
    fi

    printf "\n## Dynamic Plugin Artifacts\n\n"
    if find "${runtime_dir}" -type f \( -name "*.so" -o -name "*.dylib" -o -name "*.dll" \) \
        | grep -q .; then
      find "${runtime_dir}" -type f \( -name "*.so" -o -name "*.dylib" -o -name "*.dll" \) \
        -printf "%P\n" | sort
    else
      printf "none\n"
    fi

    printf "\n## FRM Artifacts\n\n"
    if has_frm_artifacts "${runtime_dir}"; then
      find "${runtime_dir}" -type f -name "*.frm" -printf "%P\n" | sort
    else
      printf "none\n"
    fi
  } >> "${report}"
}

append_catalog_files() {
  local catalog_file="$1"
  local report="$2"

  if [[ -z "${catalog_file}" ]]; then
    return
  fi

  {
    printf "\n## Catalog File\n\n"
    if [[ -f "${catalog_file}" ]]; then
      printf "%s\t%s bytes\n" "$(basename "${catalog_file}")" "$(stat -c %s "${catalog_file}")"
    else
      printf "none\n"
    fi

    printf "\n## Catalog Sidecars\n\n"
    if [[ -e "${catalog_file}.tmp" ]]; then
      printf "%s\n" "$(basename "${catalog_file}.tmp")"
    else
      printf "none\n"
    fi
  } >> "${report}"
}

has_frm_artifacts() {
  local runtime_dir="$1"
  find "${runtime_dir}" -type f -name "*.frm" | grep -q .
}

main "$@"
