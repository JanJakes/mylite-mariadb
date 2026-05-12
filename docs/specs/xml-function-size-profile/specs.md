# xml-function-size-profile

## Problem

The current minsize profile still links MariaDB's XML string-function
implementation for `EXTRACTVALUE()` and `UPDATEXML()`. These functions are a
rare SQL surface for MyLite's embedded default profile, and their implementation
pulls in an XPath interpreter plus many item subclasses.

This slice tests whether MyLite can omit those XML SQL functions from the
default minsize embedded library while keeping the shared XML parser that
MariaDB still uses for character-set XML loading.

## Source baseline

- MariaDB Server tag `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current MyLite size baseline after `small-builtin-plugin-profile`:
  - `libmariadbd.a`: 34,474,690 bytes,
  - stripped `mylite-open-close-smoke`: 15,849,720 bytes,
  - linked `size` total: 16,084,955 bytes.

## Source findings

- `vendor/mariadb/server/libmysqld/CMakeLists.txt` includes
  `../sql/item_xmlfunc.cc` in `SQL_EMBEDDED_SOURCES`.
- `vendor/mariadb/server/sql/item_xmlfunc.cc` implements the XPath evaluator,
  XML parsing bridge, `Item_func_xml_extractvalue::val_str()`, and
  `Item_func_xml_update::val_str()`.
- `vendor/mariadb/server/sql/item_xmlfunc.h` declares the item classes and is
  included through `vendor/mariadb/server/sql/item.h`.
- `vendor/mariadb/server/sql/item_create.cc` is the only located embedded SQL
  function registration site for `EXTRACTVALUE` and `UPDATEXML`; it declares
  `Create_func_xml_extractvalue` and `Create_func_xml_update`, defines their
  singleton instances and builders, and registers them in the native function
  array.
- `vendor/mariadb/server/strings/xml.c` is not limited to SQL XML functions.
  `vendor/mariadb/server/strings/ctype.c` still uses `my_xml_parse()` to load
  XML character-set definitions, so removing `xml.c` is not part of this
  slice.
- `vendor/mariadb/server/sql/sql_load.cc`, `sql_yacc.yy`, `sql_class.h`, and
  `lex.h` still carry `LOAD XML` support. This slice does not remove that
  parser or execution surface.
- GNU `size` on the current `item_xmlfunc.cc.o` reports 46,417 bytes of text,
  53,000 bytes of data, 36 bytes of bss, and 104,575 bytes total sections. The
  unstripped object file is 500,832 bytes on disk.
- MariaDB's official documentation describes `EXTRACTVALUE()` as extracting
  XML text matched by XPath and `UPDATEXML()` as replacing XML fragments matched
  by XPath:
  - <https://mariadb.com/docs/server/reference/sql-functions/string-functions/extractvalue>
  - <https://mariadb.com/docs/server/reference/sql-functions/string-functions/updatexml>

## Proposed design

Add an embedded-library CMake option:

```cmake
MYLITE_DISABLE_XML_FUNCTIONS
```

When enabled:

- add `-DMYLITE_DISABLE_XML_FUNCTIONS` for the embedded library sources,
- remove `../sql/item_xmlfunc.cc` from `SQL_EMBEDDED_SOURCES`,
- compile `vendor/mariadb/server/sql/item_create.cc` without the XML function
  builder classes, singleton definitions, builder methods, or native function
  array entries for `EXTRACTVALUE` and `UPDATEXML`,
- set `-DMYLITE_DISABLE_XML_FUNCTIONS=ON` in `tools/build-mariadb-minsize.sh`.

Do not remove `item_xmlfunc.h` from `item.h` in this slice. The header is
upstream shape, and no code should instantiate those item classes when the
native builders are absent.

## Non-goals

- Do not remove `strings/xml.c` or `include/my_xml.h`.
- Do not remove `LOAD XML`.
- Do not remove XML mentions from the full server target.
- Do not make a public API change.
- Do not change the generic unknown-function diagnostic beyond MariaDB's
  existing behavior after the native builders are absent.

## Affected subsystems

- Embedded library build graph.
- Native SQL function registration.
- `libmylite` open/close smoke coverage for unsupported minsize surfaces.

## DDL metadata routing impact

None. The removed surface is scalar SQL functions. It does not write table
definitions, schema objects, or file-format metadata.

## Single-file and embedded-lifecycle impact

None expected. The slice should reduce linked code without changing MyLite
storage, catalog, locks, recovery, or lifecycle ownership. The smoke test should
prove the embedded runtime still opens, executes supported SQL, rejects the XML
functions, and closes without new runtime files.

## Public API and file-format impact

No public `libmylite` API change. No `.mylite` file-format change.

## Binary-size impact

Expected savings are bounded by the linked parts of `item_xmlfunc.cc.o`.
The object section size is about 102 KiB before link/layout overhead, while the
object file is about 489 KiB on disk before archive stripping. The final
decision must use the production measurements from:

- `build/mariadb-minsize/libmysqld/libmariadbd.a`,
- `build/mariadb-minsize/mylite/mylite-open-close-smoke`,
- stripped `mylite-open-close-smoke`,
- GNU `size` output for the linked smoke binary.

## License, trademark, and dependency impact

No new dependency or license impact. The change removes MariaDB-derived embedded
code from the default minsize profile but leaves the imported source available
in the tree.

## Test plan

Run:

```sh
MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Add open/close smoke assertions that:

- `SELECT EXTRACTVALUE('<a>x</a>', '/a')` fails in the minsize profile,
- `SELECT UPDATEXML('<a>x</a>', '/a', '<b>y</b>')` fails in the minsize
  profile,
- both failures use MariaDB's unknown-function path after the native builders
  are absent.

Measure:

```sh
stat -c "%s" build/mariadb-minsize/libmysqld/libmariadbd.a
ar t build/mariadb-minsize/libmysqld/libmariadbd.a | wc -l
stat -c "%s" build/mariadb-minsize/mylite/libmylite.a
stat -c "%s" build/mariadb-minsize/mylite/mylite-open-close-smoke
cp build/mariadb-minsize/mylite/mylite-open-close-smoke \
  build/mariadb-minsize/mylite/mylite-open-close-smoke.stripped
strip --strip-unneeded \
  build/mariadb-minsize/mylite/mylite-open-close-smoke.stripped
stat -c "%s" build/mariadb-minsize/mylite/mylite-open-close-smoke.stripped
size build/mariadb-minsize/mylite/mylite-open-close-smoke
```

## Acceptance criteria

- The minsize build succeeds with `MYLITE_DISABLE_XML_FUNCTIONS=ON`.
- The linked smoke binary no longer contains `Item_func_xml_extractvalue`,
  `Item_func_xml_update`, or `my_xpath_function` symbols.
- Supported open/close, SQL execution, DML, prepared statement, read-only, URI,
  storage, and compatibility smokes pass.
- `EXTRACTVALUE()` and `UPDATEXML()` are rejected in the minsize profile.
- Size deltas are recorded in `docs/research/production-size-analysis.md`.

## Risks and unresolved questions

- Removing native function registrations changes compatibility for any
  application that depends on these XML functions. That is a deliberate minsize
  tradeoff, not a general MariaDB compatibility improvement.
- `LOAD XML` remains present, so XML-named SQL surface is not fully removed.
  Removing it would require a separate parser and `sql_load.cc` slice.
- If `item_xmlfunc.h` inline methods or typeinfo are unexpectedly pulled in by
  another translation unit, the linked savings may be smaller than object-size
  estimates. The symbol check will verify this.
