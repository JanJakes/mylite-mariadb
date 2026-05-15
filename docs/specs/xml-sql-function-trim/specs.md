# XML SQL Function Trim

## Problem

MyLite's embedded profile should keep MySQL/MariaDB application behavior that
fits the local file-owned runtime, while removing low-value compatibility
surfaces from the default archive when they are not part of the near-term
product. MariaDB's XML XPath helper functions `EXTRACTVALUE()` and
`UPDATEXML()` live behind a dedicated XML function object and native function
builders. They are not needed for current storage, metadata, catalog, or
WordPress-shaped compatibility coverage.

The bundle-size research records a successful trim for omitting
`item_xmlfunc.cc` and removing the two native builders. This slice turns that
evidence into an explicit MyLite profile option and stable public SQL policy.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/CMakeLists.txt:172` includes `item_xmlfunc.cc` in the normal
  SQL library.
- `mariadb/libmysqld/CMakeLists.txt:97` includes `../sql/item_xmlfunc.cc` in
  the embedded `sql_embedded` archive.
- `mariadb/sql/item_create.cc:2791-2814` declares
  `Create_func_xml_extractvalue` and `Create_func_xml_update`.
- `mariadb/sql/item_create.cc:6241-6255` defines the builder singletons and
  creates `Item_func_xml_extractvalue` / `Item_func_xml_update`.
- `mariadb/sql/item_create.cc:6451` registers `EXTRACTVALUE` in the native
  function array.
- `mariadb/sql/item_create.cc:6617` registers `UPDATEXML` in the native
  function array.
- `mariadb/sql/item_xmlfunc.h:129-167` declares the two public item classes.
- `mariadb/sql/item_xmlfunc.cc:3078-3110` implements their value execution.
- MariaDB documentation describes `EXTRACTVALUE(xml_frag, xpath_expr)` as an
  XML fragment plus XPath extraction helper, and `UPDATEXML(xml_target,
  xpath_expr, new_xml)` as an XML fragment replacement helper.

Official MariaDB references:

- <https://mariadb.com/kb/en/extractvalue/>
- <https://mariadb.com/docs/server/reference/sql-functions/string-functions/updatexml>

## Scope

- Add `MYLITE_WITH_XML_SQL_FUNCTIONS`, defaulting to `ON`.
- Set `MYLITE_WITH_XML_SQL_FUNCTIONS=OFF` in the MyLite embedded profile.
- When the option is off, omit `item_xmlfunc.cc` from both the normal `sql`
  target and embedded `sql_embedded` archive.
- Guard the `EXTRACTVALUE()` and `UPDATEXML()` native builders and function
  table entries behind the same option.
- Reject public direct and prepared SQL that calls `EXTRACTVALUE()` or
  `UPDATEXML()` before MariaDB execution with a stable MyLite diagnostic.

## Non-Goals

- Remove `LOAD XML`; that is already covered by the file-import policy and
  `MYLITE_WITH_LOAD_DATA`.
- Remove JSON SQL functions, JSON table functions, dynamic-column functions,
  or XML output modes.
- Remove XML parser helpers that are still reached by retained MariaDB code.
- Implement a replacement XML/XPath evaluator in MyLite.

## Design

Define `MYLITE_WITH_XML_SQL_FUNCTIONS` in `mariadb/sql/CMakeLists.txt` and
`mariadb/libmysqld/CMakeLists.txt`. Use a `MYLITE_XML_SQL_FUNCTION_SOURCE`
source variable so the default upstream-like configuration still builds
`item_xmlfunc.cc`, while the MyLite embedded profile uses an empty source list
for those functions.

In `mariadb/sql/item_create.cc`, add a default-enabled preprocessor definition
and guard:

- `Create_func_xml_extractvalue`;
- `Create_func_xml_update`;
- their singleton/create definitions;
- the `EXTRACTVALUE` and `UPDATEXML` entries in `native_func_registry_array`.

In `libmylite`, add a policy scanner for function calls to `EXTRACTVALUE()` and
`UPDATEXML()`. The scanner should use the existing token scanner pattern so
quoted strings and comments do not trigger false positives, and should require
an opening parenthesis after the function token.

## Compatibility Impact

`EXTRACTVALUE()` and `UPDATEXML()` become explicit unsupported SQL functions in
the core embedded profile. Ordinary string, JSON, date/time, numeric, and
storage-oriented SQL functions remain unchanged.

This is a compatibility tradeoff, but it is narrow: MyLite has no XML column
type, XML XPath storage behavior, or current application-schema coverage that
depends on these legacy helpers. Applications that rely on XML XPath functions
will need a future compatibility profile or application-side XML processing.

## Single-File And Embedded-Lifecycle Impact

No durable file, sidecar, startup, shutdown, or recovery behavior changes. The
trim removes unreachable XML function execution code from the default embedded
profile.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes. Public SQL execution and
prepare APIs return stable MyLite diagnostics for the unsupported functions.

## Storage-Engine Routing Impact

None. The slice affects scalar function availability before storage-engine
routing.

## Wire-Protocol Or Integration-Package Impact

None for core `libmylite`. A future compatibility adapter could enable the
option and expose these functions deliberately if an integration needs them.

## Binary-Size Impact

`docs/architecture/bundle-size-research.md` records prior evidence for omitting
`item_xmlfunc.cc` and removing the two native builders at 264,240 bytes from a
stripped linked runtime proxy and 517,000 bytes from the embedded archive. This
slice remeasured the current branch after the profile had already trimmed LOAD
execution, host-file SQL I/O, server utility functions, and Oracle SQL mode
parsing. The current default embedded archive is 245,928 bytes smaller
(`30,958,576` to `30,712,648` bytes), with one fewer member. The opt-in
storage-smoke archive has the same 245,928-byte archive reduction
(`31,139,160` to `30,893,232` bytes). Linked embedded smoke binaries are about
90 KB smaller after stripping and expose 73 fewer global symbols.

## License Or Dependency Impact

No new dependencies and no license change.

## Test And Verification Plan

- Add direct execution policy tests for `EXTRACTVALUE()` and `UPDATEXML()`.
- Add prepared-statement policy coverage for both functions.
- Verify quoted mentions of the function names still execute as ordinary
  strings.
- Reconfigure and rebuild the default embedded MariaDB archive and the
  storage-smoke MariaDB archive.
- Build and run embedded and storage-smoke presets.
- Run `tools/mylite-compat-harness report server-surface`.
- Run `tools/mylite-size-report` and archive `measure` commands.
- Run formatting, shell syntax, whitespace, normal MariaDB `sql` target, and
  first-party tidy checks.

## Acceptance Criteria

- The default MyLite embedded profile records
  `MYLITE_WITH_XML_SQL_FUNCTIONS=OFF`.
- Embedded archives omit `item_xmlfunc.cc.o`.
- Public direct and prepared entry points reject `EXTRACTVALUE()` and
  `UPDATEXML()` with stable MyLite diagnostics.
- Existing XML file-import policy for `LOAD XML` remains unchanged.
- Size documentation records the current measured impact.

## Risks And Open Questions

- Some legacy MySQL/MariaDB applications may use these XML helpers. This slice
  makes the incompatibility explicit rather than letting it fail as an unknown
  function or carrying the implementation silently.
- The slice only removes the two XML XPath SQL functions. XML-related output,
  `LOAD XML`, and future `XMLTYPE` compatibility are separate surfaces.
