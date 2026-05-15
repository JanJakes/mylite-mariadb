# Monorepo Structure

MyLite is organized as a C/C++ monorepo around a MariaDB-derived embedded
runtime. First-party code stays separate from the upstream MariaDB source so
mechanical imports and local MyLite changes remain reviewable.

## Layout

| Path | Purpose |
| --- | --- |
| `.agents/skills/` | Repo-local agent workflows for MyLite planning, implementation, review, and upstream work. |
| `.github/workflows/` | GitHub Actions CI workflows for first-party build and developer checks. |
| `CMakePresets.json` | Shared configure, build, test, and check commands for local development and CI. |
| `cmake/` | Shared CMake helpers for first-party packages and tools. |
| `docs/architecture/` | Architecture notes and engineering standards for repository-wide design decisions. |
| `docs/specs/` | Slice specifications for substantive compatibility and storage work. |
| `mariadb/` | Mechanical import of the pinned MariaDB source tree. MyLite patches land after the import commit. |
| `packages/libmylite/` | First-party embedded runtime library. This owns the public MyLite C API boundary. |
| `packages/mylite-storage/` | Internal first-party storage core. This owns MyLite catalog, page, transaction, lock, and recovery code behind the MariaDB handler glue. |
| `tests/` | Cross-package and integration tests. Package-local tests stay next to the package they exercise. |
| `tools/` | Command-line tools and developer utilities. Buildable tools live in subdirectories. |

## Build

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The `dev` preset uses Ninja, enables warnings as errors, and writes
`compile_commands.json` for developer tooling. Use `CMakeUserPresets.json` for
local machine-specific overrides; it is intentionally ignored by Git.

Run first-party developer checks with:

```sh
cmake --build --preset format-check
cmake --build --preset tidy
```

The format and tidy targets intentionally cover first-party `packages/`,
`tools/`, and `tests/` sources only. They do not rewrite or restyle imported
MariaDB files.

When running Homebrew `clang-tidy` on macOS, configure with Homebrew Clang so
the compile database and analysis toolchain agree:

```sh
LLVM_PREFIX="$(brew --prefix llvm)"
CC="$LLVM_PREFIX/bin/clang" CXX="$LLVM_PREFIX/bin/clang++" \
  PATH="$LLVM_PREFIX/bin:$PATH" cmake --fresh --preset dev
PATH="$LLVM_PREFIX/bin:$PATH" cmake --build --preset tidy
```

Run compatibility-oriented groups with:

```sh
tools/mylite-compat-harness list
tools/mylite-compat-harness run public-api storage-core
```

The harness maps compatibility surfaces to CTest labels and prepares MariaDB
embedded archives for groups that need them. See
[Compatibility Harness](compatibility-harness.md).

Build the current MariaDB embedded-library baseline with:

```sh
tools/mariadb-embedded-build all
```

The wrapper keeps MyLite-owned build orchestration outside `mariadb/`, uses the
initial cache in `cmake/mariadb-embedded-baseline.cmake`, and records
`libmariadbd.a` size evidence. The current baseline is documented in
[MariaDB Embedded Build](mariadb-embedded-build.md).

## Import Discipline

MariaDB is imported under `mariadb/` as source, not as a submodule. MyLite is a
MariaDB-derived fork and needs ordinary repository history for code search,
patch review, CI, and future upstream rebases.

Keep these changes separate:

- mechanical MariaDB imports,
- narrow MyLite patches to MariaDB-derived files,
- first-party MyLite packages, tools, tests, and docs.

Do not vendor unrelated parser or database runtimes into the scaffold. MariaDB
is the SQL parser, optimizer, metadata, and handler authority for this project.

The initial MariaDB import expands only the required Connector/C submodule under
`mariadb/libmariadb/`. Optional upstream submodule payloads stay absent until a
slice justifies them:

- `extra/wolfssl/wolfssl`,
- `storage/columnstore/columnstore`,
- `storage/maria/libmarias3`,
- `storage/rocksdb/rocksdb`,
- `wsrep-lib`.

For the MariaDB embedded baseline, configure the import with submodule updates
disabled and the absent-submodule surfaces turned off:

```sh
tools/mariadb-embedded-build configure
```
