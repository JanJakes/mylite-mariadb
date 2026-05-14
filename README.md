# MyLite

**MySQL/MariaDB without a server.**

> [!NOTE]
> **Status:** Early development.

## Overview

**MyLite** is an embedded MySQL/MariaDB drop-in built on a bundled MariaDB foundation.

At a glance:

| 💡 | ℹ️ |
| --- | --- |
| **Compatibility** | MariaDB LTS API (currently MariaDB 11.8) |
| **Storage** | Single MyLite database directory backed by MariaDB native storage |
| **Engine** | MariaDB embedded runtime with native MariaDB storage engines |
| **API** | A `libmylite` C library |
| **Validation** | Compatibility dashboard & extensive test suite |
| **Repository** | Monorepo for `libmylite`, tooling, protocol support, extensions, and integration wiring |
| **License** | GPL-2.0 (derived from MariaDB) |

## Goals

MyLite should power MySQL/MariaDB-oriented applications without modifications.
Here's a list of the main goals:

- **MySQL/MariaDB drop-in:** Work as an effortless drop-in replacement for MySQL/MariaDB.
- **Single directory:** Keep the database portable as one MyLite-owned directory.
- **Uncompromising compatibility:** Support the MySQL/MariaDB API surface that real applications depend on.
- **In-process runtime:** Execute SQL through `libmylite` without a database server.
- **Write concurrency:** Preserve MariaDB native storage-engine concurrency guarantees.
- **Extensive test suite:** Create and maintain a large test suite.
- **Small profile:** Keep the minimum necessary slice of MariaDB codebase.
- **Coverage matrix:** Track MySQL/MariaDB functionality coverage in a detailed document.

## Compatibility

MyLite targets MySQL/MariaDB compatibility where that compatibility fits an
embedded serverless runtime. MyLite makes compatibility with MySQL and MariaDB
a fundamental principle of the project. The compatibility is carefully
evaluated, tracked, and covered with tests.

See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) for the current compatibility status.

## Architecture

MyLite is built on MariaDB foundations with a small embedded integration layer.

At a glance:

| 💡 | ℹ️ |
| --- | --- |
| **MariaDB engine (`libmysqld`)** | MariaDB's `libmysqld` trimmed down to the necessary minimum. |
| **MariaDB storage** | Native MariaDB storage in a single MyLite-owned database directory. |
| **MyLite C API (`libmylite`)** | An embedded C API inspired by SQLite. |

MyLite is lean and significantly smaller than a full MariaDB build. Most notably:

- **No daemon.** MyLite is an embedded database without a server.
- **No required networking.** The core library exposes an embedded C API.
- **No server management.** MyLite doesn't need to manage and maintain a running server.
- **No exotic features.** MyLite keeps MySQL/MariaDB API broadly, but leaves out exotic features and experiments.

### SQL pipeline

1. **Parse:** MariaDB parses supported SQL syntax.
2. **Analyze:** MariaDB resolves all statement semantics under MyLite's embedded engine.
3. **Plan:** MariaDB chooses execution plans over native engine tables and indexes.
4. **Execute:** MariaDB's native storage engines directly use the MyLite database directory.

### Storage layout

MyLite keeps MariaDB's native storage engines and constrains their file layout
with the following properties:

- **Single directory:** Durable database state is stored inside one
  MyLite-owned directory.
- **Native engine files:** InnoDB, MyISAM, Aria, and other supported engines use
  their MariaDB-native file formats.
- **No external durable state:** All MariaDB data and companion files must stay
  inside the MyLite database directory.
- **Safety:** Transaction and crash recovery guarantees come from MariaDB's
  native storage engines.
- **Concurrency:** Write concurrency follows the selected MariaDB storage
  engine's native behavior.

The current goal is to support MariaDB-native `InnoDB`, `MyISAM`, `Aria`, and
`MEMORY` inside the MyLite database directory. Other zero-file or optional
engines belong in the default profile only when their build shape, ownership,
and compatibility behavior are designed and tested.

The directory path must be configurable, similarly to SQLite database file location.
The key difference is that MyLite's directory can hold multiple databases, following
MariaDB engine capabilities.

MyLite should also be capable of setting up a full in-memory database regardless
of what engines are defined for table storage in the SQL. This should be
supported via the special `:memory:` database path.

### Integration packages

The repository is intended to hold the core library, as well as some surrounding
integration packages. The main components are:

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
