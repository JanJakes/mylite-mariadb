#!/usr/bin/env bash
set -euo pipefail

main() {
  if [[ "${1:-}" == "--inside-container" ]]; then
    audit_inside_container
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
    --env "MYLITE_AUDIT_TOP=${MYLITE_AUDIT_TOP:-40}" \
    "${image}" \
    /work/tools/audit-mylite-bundle.sh --inside-container
}

audit_inside_container() {
  local build_dir="${MYLITE_MARIADB_BUILD_DIR:-build/mariadb-minsize}"
  local top="${MYLITE_AUDIT_TOP:-40}"
  local probe_dir="${build_dir}/mylite"
  local libmylite="${probe_dir}/libmylite.a"
  local libmariadbd="${build_dir}/libmysqld/libmariadbd.a"
  local source="${probe_dir}/mylite-php-probe.audit.cc"
  local version_script="${probe_dir}/mylite-php-probe.audit.version"
  local probe="${probe_dir}/libmylite-php-probe.audit.so"
  local stripped="${probe}.stripped"
  local sectionless="${probe}.sectionless"
  local map="${probe}.map"
  local report="${build_dir}/mylite-bundle-audit-report.md"

  require_file "${libmylite}"
  require_file "${libmariadbd}"
  mkdir -p "${probe_dir}"

  write_probe_source "${source}" "${version_script}"
  link_probe "${source}" "${version_script}" "${probe}" "${map}" \
    "${libmylite}" "${libmariadbd}"
  strip_probe "${probe}" "${stripped}" "${sectionless}"
  local audit_status=0
  write_report "${report}" "${build_dir}" "${probe}" "${stripped}" \
    "${sectionless}" "${map}" "${libmylite}" "${libmariadbd}" "${top}" \
    || audit_status=$?

  printf "Bundle audit report: %s\n" "${report}"
  return "${audit_status}"
}

require_file() {
  local path="$1"

  if [[ ! -f "${path}" ]]; then
    printf "Missing required artifact: %s\n" "${path}" >&2
    printf "Build first, for example:\n" >&2
    printf "  MYLITE_MARIADB_BUILD_DIR=%q tools/build-mariadb-minsize.sh\n" \
      "${MYLITE_MARIADB_BUILD_DIR:-build/mariadb-minsize}" >&2
    return 1
  fi
}

write_probe_source() {
  local source="$1"
  local version_script="$2"

  cat > "${source}" <<'EOF'
#include "mylite.h"

extern "C" __attribute__((visibility("default")))
int mylite_php_probe(const char *path)
{
  mylite_db *db= nullptr;
  int rc= mylite_open(path, &db);
  if (db)
    rc|= mylite_close(db);
  return rc;
}
EOF

  cat > "${version_script}" <<'EOF'
MYLITE_PHP_PROBE_1.0 {
  global: mylite_php_probe;
  local: *;
};
EOF
}

link_probe() {
  local source="$1"
  local version_script="$2"
  local probe="$3"
  local map="$4"
  local libmylite="$5"
  local libmariadbd="$6"
  local build_include
  local map_path
  local version_script_path

  build_include="$(work_path "$(dirname "${libmylite}")/../include")"
  map_path="$(work_path "${map}")"
  version_script_path="$(work_path "${version_script}")"

  /usr/bin/c++ \
    -shared \
    -fPIC \
    -fstack-protector \
    --param=ssp-buffer-size=4 \
    -moutline-atomics \
    -Oz \
    -DNDEBUG \
    -DDBUG_OFF \
    -fvisibility=hidden \
    -I/work/vendor/mariadb/server/mylite/include \
    -I"${build_include}" \
    -I/work/vendor/mariadb/server/include \
    -fuse-ld=lld \
    -Wl,-O2 \
    -Wl,-z,pack-relative-relocs \
    -Wl,--pack-dyn-relocs=relr \
    -Wl,--no-eh-frame-hdr \
    -Wl,--gc-sections \
    -Wl,--icf=all \
    -Wl,-z,relro,-z,now \
    "-Wl,--version-script=${version_script_path}" \
    "-Wl,-Map=${map_path}" \
    -o "${probe}" \
    "${source}" \
    "${libmylite}" \
    "${libmariadbd}" \
    -lm \
    -ldl
}

work_path() {
  local path="$1"

  if [[ "${path}" == /* ]]; then
    printf "%s\n" "${path}"
  else
    printf "/work/%s\n" "${path}"
  fi
}

strip_probe() {
  local probe="$1"
  local stripped="$2"
  local sectionless="$3"

  cp "${probe}" "${stripped}"
  strip --strip-unneeded "${stripped}"

  cp "${probe}" "${sectionless}"
  strip --strip-unneeded --strip-section-headers "${sectionless}"
}

write_report() {
  local report="$1"
  local build_dir="$2"
  local probe="$3"
  local stripped="$4"
  local sectionless="$5"
  local map="$6"
  local libmylite="$7"
  local libmariadbd="$8"
  local top="$9"
  local status=0
  local unused_deps
  local forbidden_symbols
  local export_count

  unused_deps="$(mktemp)"
  forbidden_symbols="$(mktemp)"

  {
    printf "# MyLite Bundle Audit\n\n"
    printf "Generated: %s\n" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf "Build directory: %s\n" "${build_dir}"
    printf "Probe: %s\n\n" "${probe}"

    printf "## Artifact Sizes\n\n"
    printf "| Artifact | Bytes | MiB |\n"
    printf "| --- | ---: | ---: |\n"
    print_size_row "${probe}"
    print_size_row "${stripped}"
    print_size_row "${sectionless}"
    print_size_row "${libmylite}"
    print_size_row "${libmariadbd}"
    printf "\n"

    printf "## File Type\n\n"
    printf '```text\n'
    file "${probe}" "${stripped}" "${sectionless}"
    printf '```\n\n'

    printf "## Dynamic Dependencies\n\n"
    printf '```text\n'
    if ! readelf -d "${probe}" | grep "NEEDED"; then
      printf "none\n"
    fi
    printf '```\n\n'

    printf "## Exported Dynamic Symbols\n\n"
    printf '```text\n'
    if ! nm -D --defined-only "${probe}"; then
      printf "none\n"
    fi
    printf '```\n\n'
    export_count="$(nm -D --defined-only "${probe}" | wc -l)"
    if [[ "${export_count}" -ne 1 ]] ||
      ! nm -D --defined-only "${probe}" | grep -q "mylite_php_probe"; then
      status=1
      printf "export_check=fail\n\n"
    else
      printf "export_check=pass\n\n"
    fi

    printf "## Unused Dynamic Dependencies\n\n"
    ldd -u -r "${probe}" > "${unused_deps}" 2>&1 || true
    printf '```text\n'
    if [[ -s "${unused_deps}" ]]; then
      cat "${unused_deps}"
      status=1
    else
      printf "none reported by ldd -u -r\n"
    fi
    printf '```\n\n'

    printf "## Largest Linked Sections\n\n"
    printf '```text\n'
    size -A -d "${probe}" | sort -k2 -nr | head -n "${top}" || true
    printf '```\n\n'

    printf "## Retained Archive Contribution\n\n"
    printf '```text\n'
    archive_contribution "${map}"
    printf '```\n\n'

    printf "## Largest Retained Objects\n\n"
    printf '```text\n'
    retained_objects "${map}" | head -n "${top}" || true
    printf '```\n\n'

    printf "## Largest Retained Symbols\n\n"
    printf '```text\n'
    nm -S --size-sort --radix=d "${probe}" | tail -n "${top}" | tac | c++filt
    printf '```\n\n'

    printf "## Removed Client C API Symbol Check\n\n"
    removed_client_symbol_matches "${probe}" > "${forbidden_symbols}" || true
    printf '```text\n'
    if [[ -s "${forbidden_symbols}" ]]; then
      cat "${forbidden_symbols}"
      status=1
    else
      printf "none\n"
    fi
    printf '```\n\n'

    printf "## Server-Surface Watchlist\n\n"
    printf '```text\n'
    server_surface_watchlist "${probe}" | head -n "${top}" || true
    printf '```\n\n'

    printf "## Audit Result\n\n"
    printf "status=%s\n" "${status}"
  } > "${report}"

  rm -f "${unused_deps}" "${forbidden_symbols}"
  return "${status}"
}

print_size_row() {
  local path="$1"
  local bytes

  bytes="$(stat -c "%s" "${path}")"
  printf '| `%s` | %s | ' "${path}" "${bytes}"
  awk -v bytes="${bytes}" 'BEGIN { printf "%.2f", bytes / 1048576 }'
  printf " |\n"
}

archive_contribution() {
  local map="$1"

  perl -ne '
    if (/^\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S*\.a)\(([^)]+)\):(?:\(|$)/) {
      my $size = hex($1);
      my $archive = $2;
      $archive =~ s#^.*/##;
      $s{$archive} += $size;
    }
    END {
      for my $k (sort { $s{$b} <=> $s{$a} } keys %s) {
        printf "%10d %s\n", $s{$k}, $k;
      }
    }
  ' "${map}"
}

retained_objects() {
  local map="$1"

  perl -ne '
    if (/^\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S*(?:\.a\([^)]+\)|\.o)):(?:\(|$)/) {
      my $size = hex($1);
      my $in = $2;
      my $key;
      if ($in =~ /\.a\(([^)]+)\)/) {
        $key = $1;
      } else {
        $key = $in;
        $key =~ s#^.*/##;
      }
      $s{$key} += $size;
    }
    END {
      for my $k (sort { $s{$b} <=> $s{$a} } keys %s) {
        printf "%10d %s\n", $s{$k}, $k;
      }
    }
  ' "${map}"
}

removed_client_symbol_matches() {
  local probe="$1"

  nm -A "${probe}" | c++filt | grep -E \
    "[[:space:]](mysql_server_init|mysql_server_end|mysql_init|mysql_real_connect|mysql_close|mysql_real_query|mysql_store_result|mysql_fetch_row|mysql_free_result|mysql_stmt_[A-Za-z0-9_]*|embedded_methods|emb_advanced_command|free_old_query|net_clear_error)([[:space:](]|$)" \
    || true
}

server_surface_watchlist() {
  local probe="$1"

  nm -S --size-sort --radix=d "${probe}" | c++filt | grep -E \
    "mysql_bin_log|plugin_init|my_long_options|schema_tables|Show::|ha_innobase|innobase|ha_myisam|maria_|EVP_|SSL_|vio_ssl|dlopen|mysql_client_plugin" \
    || true
}

main "$@"
