# MyLite

**MySQL/MariaDB in a single file.**

> [!NOTE]
> **Status:** Early development.

## Overview

**MyLite** is an embedded MySQL/MariaDB drop-in built on a bundled MariaDB foundation.

At a glance:

| 💡 | ℹ️ |
| --- | --- |
| **Compatibility** | MariaDB LTS API (currently MariaDB 11.8) |
| **Storage** | Single `.mylite` file + transient MyLite-owned companions |
| **Engine** | MariaDB embedded runtime with a custom MyLite storage engine |
| **API** | A `libmylite` C library |
| **Validation** | Compatibility dashboard & extensive test suite |
| **Repository** | Monorepo for `libmylite`, tooling, protocol support, extensions, and integration wiring |
| **License** | GPL-2.0 (derived from MariaDB) |

## Goals

MyLite should power MySQL/MariaDB-oriented applications without modifications.
Here's a list of the main goals:

- **MySQL/MariaDB drop-in:** Work as an effortless drop-in replacement for MySQL/MariaDB.
- **Single file:** Keep the database portable as a single `.mylite` file.
- **Uncompromising compatibility:** Support the MySQL/MariaDB API surface that real applications depend on.
- **In-process runtime:** Execute SQL through `libmylite` without a database server.
- **Write concurrency:** Implement full write concurrency support in the MyLite storage.
- **Extensive test suite:** Create and maintain a large test suite.
- **Small profile:** Keep the minimum necessary slice of MariaDB codebase.
- **Coverage matrix:** Track MySQL/MariaDB functionality coverage in a detailed document.

## Compatibility

MyLite targets MySQL/MariaDB compatibility where that compatibility fits an
embedded single-file runtime. MyLite makes compatibility with MySQL and MariaDB
a fundamental principle of the project. The compatibility is carefully
evaluated, tracked, and covered with tests.

See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) for the current compatibility status.

## Architecture

MyLite is built on MariaDB foundations with some custom key components.

At a glance:

| 💡 | ℹ️ |
| --- | --- |
| **MariaDB (`libmysqld`)** | MariaDB's `libmysqld` trimmed down to the necessary minimum. |
| **MyLite C API (libmylite)** | An embedded C API inspired by SQLite. |
| **MyLite storage** | A new single-file storage layer inspired by SQLite. |

MyLite is lean and significantly smaller than a full MariaDB build. Most notably:

- **No daemon.** MyLite is an embedded database without a server.
- **No required networking.** The core library exposes an embedded C API.
- **No server management.** MyLite doesn't need to manage and maintain a running server.
- **No exotic features.** MyLite keeps MySQL/MariaDB API broadly, but leaves out exotic features and experiments.

### SQL pipeline

1. **Parse:** MariaDB parses supported SQL syntax.
2. **Analyze:** MariaDB resolves metadata, types, functions, warnings, errors,
   and statement semantics under MyLite's embedded configuration.
3. **Plan:** MariaDB chooses execution plans over MyLite engine tables and
   indexes.
4. **Execute:** The MyLite storage engine provides data from the `.mylite` file.

### Storage engine

MyLite implements a custom storage engine with the following properties:

- **Single file:** All database data is stored in a single `.mylite` file.
- **Companion files:** Companion files can be used, but only for journals, WAL, locks, shared memory, or temporary scratch work when part of the MyLite file lifecycle.
- **Safety:** The storage format must provide transaction and crash recovery guarantees.
- **Concurrency:** The storage must support full write concurrency.

Major MySQL and MariaDB storage engines will be routed to the MyLite custom storage implementation. The current goal is to support `InnoDB`, `MyISAM`, and `Aria`. Zero-file storage engines like `MEMORY` and `BLACKHOLE` should be supported as well.

MyLite should also be capable of setting up a full in-memory database regardless of what engines are defined for table storage in the SQL. This should be supported via the special `:memory:` database filename.

### Integration packages

The repository is intended to hold the core library, as well as some surrounding integration packages. The main components are:

- The `libmylite` library code.
- MySQL/MariaDB wire protocol support.
- Command-line tooling.
- Language and runtime integration packages.
- Test suites.

## Development

The repository uses CMake presets for first-party packages and tools:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The MariaDB-backed `libmylite` lifecycle tests use the embedded archive built by
the MariaDB baseline wrapper:

```sh
tools/mariadb-embedded-build all
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev
```

The opt-in storage-engine smoke uses a separate MariaDB archive so the default
embedded baseline does not enable the MyLite handler:

```sh
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all \
  -DPLUGIN_MYLITE_SE=STATIC
cmake --preset storage-smoke-dev
cmake --build --preset storage-smoke-dev
ctest --preset storage-smoke-dev
```

Compatibility-oriented groups can be run through the MyLite harness:

```sh
tools/mylite-compat-harness list
tools/mylite-compat-harness run public-api storage-core
tools/mylite-compat-harness report embedded-lifecycle sidecar routed-ddl-dml
```

The opt-in MariaDB MTR smoke runner builds extra upstream test tools and runs a
small curated embedded MTR subset:

```sh
tools/mylite-mtr-harness list
tools/mylite-mtr-harness run
```

The storage-smoke performance baseline is local, machine-dependent evidence for
before/after storage work, with direct/prepared timings and scan-fallback vs.
published-leaf secondary index reads labelled separately:

```sh
tools/mylite-perf-baseline
tools/mylite-perf-baseline 1000 1000
tools/mylite-perf-baseline --phase=prepared-scalar-selects 1000 10000
tools/mylite-perf-baseline --phase=storage-pk-entry-lookups 1000 10000
tools/mylite-perf-baseline --phase=storage-pk-entry-lookups-one-read 1000 10000
tools/mylite-perf-baseline --phase=storage-pk-row-lookups 1000 10000
tools/mylite-perf-baseline --phase=storage-pk-row-lookups-one-read 1000 10000
tools/mylite-perf-baseline --phase=storage-read-statements 1000 10000
tools/mylite-perf-baseline --phase=storage-row-updates 1000 10000
tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000
tools/mylite-perf-baseline --phase=prepared-pk-select-components 1000 10000
tools/mylite-perf-baseline --phase=prepared-pk-select-reset-after-row 1000 10000
tools/mylite-perf-baseline --phase=prepared-secondary-selects 1000 10000
tools/mylite-perf-baseline --phase=prepared-leaf-secondary-selects 1000 10000
tools/mylite-perf-baseline --phase=updates 1000 10000
tools/mylite-perf-baseline --phase=prepared-updates 1000 10000
tools/mylite-perf-baseline --phase=prepared-update-components 1000 10000
tools/mylite-perf-baseline --phase=prepared-updates --max-us=prepared-updates:25 1000 10000
```

Set `MYLITE_PERF_KEEP_ROOT=1` when investigating a failed benchmark run and
the generated temporary `.mylite` file should be preserved for inspection.
Thresholds are opt-in and machine-local; use `--max-us=<metric>:<value>` for
explicit regression gates.

See [docs/architecture/monorepo.md](docs/architecture/monorepo.md) for the
repository layout and import discipline.

## References

- [MariaDB Server](https://github.com/MariaDB/server) is the upstream source
  repository.
- [MariaDB releases](https://mariadb.org/mariadb/all-releases/) list supported
  release lines and release status.
- [MariaDB source-code documentation](https://mariadb.com/docs/server/clients-and-utilities/server-client-software/download/getting-the-mariadb-source-code)
  explains how to get the MariaDB source.
- [Compiling MariaDB from source](https://mariadb.com/docs/server/server-management/install-and-upgrade-mariadb/compiling-mariadb-from-source/compiling-mariadb-from-source-the-master-guide)
  documents the upstream build process.
- [Embedded MariaDB interface](https://mariadb.com/docs/general-resources/development-articles/mariadb-internals/using-mariadb-with-your-programs-api/libmysqld/embedded-mariadb-interface)
  describes MariaDB's embedded server interface.
- [MariaDB storage-engine overview](https://mariadb.com/docs/server/server-usage/storage-engines/storage-engines-storage-engines-overview)
  introduces MariaDB storage engines.
- [MariaDB licensing FAQ](https://mariadb.com/docs/general-resources/community/community/faq/licensing-questions/licensing-faq)
  explains MariaDB's GPL licensing.

## License

MyLite is GPL-2.0 because it is derived from MariaDB.
