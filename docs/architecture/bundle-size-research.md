# Bundle size reduction attempts

This document is a standalone ranked inventory of the bundle-size research on
the `bundle-size` branch. It consolidates the branch commit history, the size
specs, and the production-size analysis into one table without linking out to
the individual specs.

## Measurement model

- The ranking uses incremental savings, not cumulative savings. Cumulative
  deltas would mostly rank later rows highest because they include earlier
  work.
- For rows in the applied size stack, the incremental saving is the difference
  from the immediately preceding measured stack row in the production-size
  analysis.
- The default sort basis is the stripped linked MyLite open-close smoke binary,
  which is the most consistently measured linked-artifact proxy across the
  branch.
- Rows with no linked-artifact measurement are marked `n/a` for bundle saving
  and placed by their archive saving. Rows with a measured linked artifact that
  did not shrink keep a `0` bundle saving and sort with other zero-effect linked
  rows.
- Rejected, superseded, and opt-in rows with comparable artifact measurements
  remain ranked by their measured effect and are marked in the outcome. Failed
  or unbuilt rows with no comparable artifact measurement are placed at the end.
- Positive numbers are bytes saved. Negative numbers are growth.
- Archive savings are shown separately because some attempts reduce a
  distributable static archive without changing the linked bundle proxy.
- Dynamic dependency removals are described in the outcome when they matter.
  They are not the default sort key because MyLite has not chosen whether final
  packages vendor system libraries.
- Measurement-only findings that do not represent a ranked saving lever are
  summarized after the ranked table.

The original production-size comparison baseline used here was:

| Artifact | Bytes | MiB |
| --- | ---: | ---: |
| `libmariadbd.a` | 43,405,432 | 41.39 |
| stripped linked open-close smoke | 19,331,904 | 18.44 |

The most reduced research-stack measurement captured on the source branch was:

| Artifact | Bytes | MiB |
| --- | ---: | ---: |
| `libmariadbd.a` | 22,444,096 | 21.40 |
| stripped linked open-close smoke | 3,982,696 | 3.80 |
| corrected PHP-shaped shared probe | 3,861,728 | 3.68 |
| sectionless corrected PHP-shaped shared probe | 3,859,368 | 3.68 |

## Current default-profile review

This section compares the historical `bundle-size` research against the current
default embedded profile on `main` as of 2026-05-21. The current profile is
more conservative than the most reduced research stack: it keeps native storage
direction, prepared statements, JSON, GEOMETRY/GIS, and broad
charset/collation coverage. It also avoids treating planned SQL surfaces such
as views, triggers, routines, FULLTEXT, SPATIAL, and sequence behavior as safe
default size cuts.

The current default archive is measured in
[mariadb-embedded-build.md](mariadb-embedded-build.md) at 25,635,600 bytes /
24.45 MiB after post-build archive stripping. A current one-export macOS
shared-object probe measured outside the committed build scripts was
14,188,976 bytes / 13.53 MiB stripped. That probe is useful directional
evidence, but it is not directly comparable to the 3.86 MiB historical
research-stack probe because the current default intentionally retains more SQL
and compatibility surface.

Status labels:

- **Applied** means the current default profile already contains the same
  reduction, a narrower safer version, or a superseding implementation.
- **Still safe to evaluate** means the row appears compatible with the current
  product direction, but should be remeasured and tested on the current
  baseline before it becomes default.
- **Needs decision** means the row may fit a specialized profile or packaging
  target, but it changes compatibility, portability, hardening, observability,
  or architecture enough that it should not be treated as a default safe cut.
- **Not planned for default** means the row removes functionality that is
  important to the current MyLite direction.
- **Historical only** means the row failed, was superseded, had no useful size
  effect, or exists only as measurement evidence.

### Applied in the current default profile

These rows are represented in the current default embedded profile:

| Rows | Current status |
| --- | --- |
| 7, 9, 115 | Covered by the current platform-specific archive stripping and `ranlib` step after relink verification. The current Darwin path uses a narrower release-archive strip policy than the riskiest one-off research command. |
| 10, 12, 27, 28, 31 | Applied through the Oracle parser stub, embedded-target exception cut, `SFORMAT()` removal, unwind-table omission, and SQL `HELP` unsupported-command stub. |
| 15, 38, 39, 40, 41, 44, 53, 55, 58, 60, 61, 67, 69, 72, 77, 78, 79, 90, 91, 92, 94, 97, 100, 108, 112, 114, 116, 117, 126 | Applied to server-oriented, host-file, diagnostic, option-help, unsupported-command, and policy surfaces that are outside the core embedded library contract. |
| 20 | Applied with compact server error messages while preserving useful MyLite-facing diagnostics. |
| 29, 34, 71, 75, 83, 85, 104, 105, 106, 107, 123 | Applied through the no-binlog, no-replication, and no-external-XA default profile. |
| 46 | Applied for persistent optimizer statistics and JSON histogram storage while preserving ordinary planning, `ANALYZE TABLE`, and `EXPLAIN`. |
| 50 | Applied for dynamic plugin shared-object loading. Static built-in plugins and native storage remain available. |
| 52 | Partly applied by trimming network-auth client handshake support and inherited daemon/client startup roots. Broader embedded-client C API root removal remains a separate architecture decision. |
| 102 | Applied for vector SQL functions, MHNSW runtime, and mandatory `mhnsw` plugin registration. The `VECTOR(N)` type is intentionally retained. |
| 103 | Partly applied for `user_variables` and user-stat diagnostics. The sequence surface is retained because simple sequence behavior is now covered, and remaining plugin choices should be deliberate. |

### Still safe to evaluate

These rows look compatible with the current direction and are good candidates
for follow-up measurement, with the caveats shown:

| Rows | Why still plausible |
| --- | --- |
| 1 | `DT_RELR` can be a large Linux linked-artifact win if the package can require a modern glibc loader. Treat it as a Linux packaging candidate, not a cross-platform default. |
| 5 | Removing accidental executable exports is safe where any current probe or package target still exports the full linked SQL symbol set. The current tree should first get a committed PHP/shared-object audit target. |
| 14 | Function/data sections plus linked-artifact section GC are safe for final linked packages, but grow the static archive. This should be a package-target option, not an archive-size metric. |
| 22, 89 | Clang/lld and lld `-O2` are packaging candidates. They need a current rerun on the supported platform matrix because previous measurements predated direct sessions and later cuts. |
| 23 | `--no-eh-frame-hdr` kept exception metadata but reduces unwind lookup metadata. Safe to benchmark for release packages, with debugger/unwinder expectations documented. |
| 47 | Hidden visibility defaults or an export map are still appropriate for a final shared library if the public `MYLITE_API` surface is audited first. |
| 88 | Bypassing inherited option-file loading after MyLite's required `--no-defaults` startup is probably safe, but should be proven against the current embedded bootstrap and config-file policy. |
| 98 | Section-header stripping is safe only for final release artifacts where post-link debugging and inspection are not required. Keep debug/release artifacts separate. |
| 109 | The `tpool_wait_begin()` / `tpool_wait_end()` no-op cleanup is semantically safe but tiny. It should be done only if it keeps inherited thread-pool hooks clearer. |
| 111 | `-Oz` is safe to benchmark, but the historical win was negligible. |
| 122 | A committed PHP-extension-shaped shared-object audit is still useful. This is measurement tooling, not a reduction. |

### Needs product or packaging decision

These rows are not obviously wrong, but they require an explicit decision before
becoming default:

| Rows | Decision needed |
| --- | --- |
| 18 | Identical code folding is a linked-artifact win, but `--icf=all` can merge functions with distinct addresses. Decide whether that is acceptable for the final ABI and diagnostics. |
| 21, 32, 45, 56 | MyISAM temp-spill, repair/check/optimize, CSV/MRG_MyISAM, key-cache, and maintenance cuts interact with native-storage behavior and admin compatibility. They need a storage-surface policy before being default cuts. |
| 24, 57, 64, 65, 68, 82 | Stored routines, triggers, events, views, PL/SQL cursor attributes, and routine metadata are planned or adjacent to planned SQL surfaces. They may be deferred, but should not be permanently trimmed without a roadmap decision. |
| 35 | An `en_US`-only locale table is a compatibility decision for date/time formatting and `lc_time_names`. |
| 43 | Removing `EXPLAIN`, `ANALYZE`, and `SHOW EXPLAIN` hurts developer and application introspection. The current profile intentionally keeps `EXPLAIN`. |
| 51, 66, 74, 80, 87, 95, 124 | Crypto, compression, encryption hooks, and OpenSSL wrapper cuts need a security and compatibility policy. They may become profile-specific, especially if dependency bundling dominates package size. |
| 73, 101 | Direct embedded result adapters are architecture work, not simple size cuts. They are likely useful for the final PHP-like shape, but must preserve the public prepared/query API behavior. |
| 76 | Removing the retained `VECTOR(N)` type is a product decision. The current default trims vector functions and indexes but keeps the type parser boundary. |
| 86 | Replacing named time-zone tables with `SYSTEM` and numeric offsets only changes SQL date/time semantics. |
| 96 | System-versioned table support is an application-visible MariaDB feature. Decide whether it belongs in the default compatibility target. |
| 125 | Shrinking the charset registry was a memory-footprint experiment, not a stripped-size win. Revisit only as memory work. |

### Not planned for the default profile

These rows remove functionality that is important to the current default
direction and should not be pursued as safe size reductions:

| Rows | Reason |
| --- | --- |
| 2, 6, 17, 93 | Broad charset and collation coverage is important for MySQL/MariaDB compatibility. Smaller charset profiles can be specialized builds, not the default. |
| 11, 13, 37, 42, 54 | GEOMETRY/GIS, spatial typing, and SPATIAL index behavior remain important compatibility targets. |
| 19, 36, 49, 70 | JSON is explicitly retained. JSON histogram storage is already trimmed separately, but user-facing JSON SQL behavior should remain. |
| 26 | Window functions are ordinary SQL compatibility, not server-only surface. |
| 33, 99 | Public and SQL-language prepared statements are important for application compatibility and the `libmylite` API direction. |
| 48, 84 | FULLTEXT behavior is planned where the retained native engines support it. |
| 59 | `REGEXP`/PCRE is ordinary SQL compatibility and should stay in the default profile. |
| 62 | Sequence behavior is now partially supported and covered by tests. |
| 63 | `SHOW CREATE` output is core introspection for schema-management tools. |
| 81 | SQL diagnostics are normal SQL behavior; trimming them would be a compatibility cut, not a server-only cut. |

### Historical only

These rows should remain as research evidence but should not drive new work as
written:

| Rows | Reason |
| --- | --- |
| 3, 4 | Failed charset-small attempts, superseded by later research. |
| 8, 120 | LTO variants grew or failed the static-archive/package goals. |
| 16 | Superseded by the later temp-spill experiment and incompatible with the current native-storage caution. |
| 25, 30 | Small or superseded plugin/charset experiments whose tradeoffs were not worth accepting. |
| 110, 127 | Hardening reductions were rejected and should stay rejected. |
| 113, 118, 119, 121 | No useful size effect or superseded by better measurements. |
| 128, 129, 130 | Broad SSL, RTTI, and exception removals failed or were replaced by narrower staged cuts. |

## Ranked attempts

| Rank | Required change | Bundle saving | Archive saving | Outcome |
| ---: | --- | ---: | ---: | --- |
| 1 | Link runtime artifacts with lld `-z pack-relative-relocs` and `--pack-dyn-relocs=relr`. | 4,072,080 | 0 | Passed current smokes. Largest linked-runtime win, but requires a modern glibc loader because the linked artifact uses `DT_RELR`. |
| 2 | Set `WITH_EXTRA_CHARSETS=none` and skip generated UCA 1400 registration for charsets absent from the small profile. | 2,495,240 | 2,584,666 | Passed current smokes. This retained MariaDB's default UCA collation at that point; non-default charset coverage remains a high compatibility tradeoff. |
| 3 | Set `WITH_EXTRA_CHARSETS=none` without the later UCA registration fix. | 2,495,240 | 2,584,650 | Rejected. The build linked, but the open-close smoke segfaulted. Superseded by the later fixed charset-small build profile. |
| 4 | Set `WITH_EXTRA_CHARSETS=none` and switch the default collation to `utf8mb4_general_ci` without fixing UCA registration. | 2,495,240 | 2,584,658 | Rejected. Changing the default collation did not fix the startup segfault. Superseded by the later fixed charset-small build profile. |
| 5 | Remove `ENABLE_EXPORTS TRUE` from MyLite-owned smoke executables so they stop exporting the full linked SQL symbol set. | 2,162,688 | 0 | Passed current smokes. Linked-artifact win only; static archive size is unchanged. |
| 6 | Make UCA collations optional, omit UCA 0900 and UCA 1400 generated data, and use `utf8mb4_general_ci` as the aggressive-profile default. | 1,552,864 | 1,777,908 | Passed smokes and harness. Major compatibility tradeoff for Unicode collation coverage. |
| 7 | Run `strip --strip-unneeded` and `ranlib` on a copied release static archive. | n/a | 1,532,384 | One-off archive-only measurement before the build-script strip landed. Higher risk than `strip -g`, later accepted after smokes relinked and passed. |
| 8 | Enable CMake/GCC LTO for the minsize build. | 1,315,712 | -299,075,078 | Rejected. The stripped linked smoke shrank, but the static archive grew to 342,480,510 bytes and GCC emitted ODR/type warnings. |
| 9 | Strip debug symbols from a copied release static archive with `strip -g`. | n/a | 1,144,216 | One-off archive-only measurement. Lower-risk fallback for static archive distribution. |
| 10 | Remove the generated Oracle parser object and link an unsupported Oracle-mode stub. | 589,824 | 1,412,822 | Passed current smokes. Oracle SQL mode becomes an explicit unsupported surface. |
| 11 | Omit GIS SQL function sources and link a small empty GIS registry/type-constructor shim. | 463,440 | 864,782 | Passed current smokes. GEOMETRY/SPATIAL rejection paths remained covered. |
| 12 | Compile only the retained `sql_embedded` target with `-fno-exceptions` after removing retained SQL exception users such as `SFORMAT()`. | 458,256 | 2,554,764 | Passed smokes and harness. First-party MyLite API and storage code remain exception-capable. |
| 13 | Make `type_geom`, `type_inet`, and `type_uuid` plugins non-mandatory and disable them in the minsize profile. | 396,104 | 3,463,834 | Passed current smokes. Large archive win with SQL type compatibility cost. |
| 14 | Compile embedded targets with per-function/per-data sections and link runtime artifacts with `--gc-sections`. | 328,176 | -4,413,456 | Passed smokes and harness. Linked runtime shrank, but the static archive grew from section metadata. |
| 15 | Omit `item_xmlfunc.cc` and remove native builders for `EXTRACTVALUE()` and `UPDATEXML()`. | 264,240 | 517,000 | Passed current smokes. XML functions fail through the unknown-function path. |
| 16 | Omit MyISAM temporary-table spill support before fixing schema-table MEMORY compatibility. | 246,680 | 695,536 | Superseded. Open-close passed, but storage/catalog harness failed. |
| 17 | Stop registering compiled `general1400` collations and omit unused extended Unicode casefold tables. | 215,408 | 238,392 | Passed smokes and harness. Another collation compatibility tradeoff. |
| 18 | Enable lld identical code folding with `--icf=all`. | 163,040 | 0 | Passed smokes and harness. Link-only win with function address-identity risk. |
| 19 | Omit ordinary JSON SQL function and JSON aggregate runtime while keeping internal JSON helpers. | 137,696 | 658,858 | Passed smokes and harness. JSON SQL functions become unsupported or unknown. |
| 20 | Replace the generated full English server error catalog with a compact catalog and generic fallback. | 135,704 | 198,032 | Passed smokes and harness. Common MyLite-facing diagnostics are preserved. |
| 21 | Omit the mandatory MyISAM temp-spill engine, bound schema-table metadata for MEMORY, and reject disk temp spill explicitly. | 117,152 | 419,960 | Passed smokes and harness. Removes a remaining inherited temp-file engine. |
| 22 | Build runtime artifacts with Clang/Clang++ 18, `WITH_PIC=ON`, global-dynamic TLS, and lld `-O2`. | 115,176 | -4,386,070 | Passed smokes before direct sessions. The lld-`O2` PHP-shaped shared probe was 3,783,264 bytes stripped, 78,464 bytes smaller than the current corrected GCC shared probe. Static archive growth keeps this a packaging-only candidate that needs rerun on top of direct sessions. |
| 23 | Link runtime artifacts with lld `--no-eh-frame-hdr`. | 96,824 | 0 | Passed smokes and harness. Keeps `.eh_frame` and `.gcc_except_table`. |
| 24 | Replace stored-program compiler/runtime objects with a fail-closed embedded stub. | 93,712 | 626,652 | Passed smokes and harness. Stored routine, trigger, event, and package compilation fail explicitly. |
| 25 | Configure MariaDB with `WITH_EXTRA_CHARSETS=complex`. | 83,536 | 80,240 | Rejected. Savings were too small to justify the charset compatibility profile. |
| 26 | Omit dedicated window-function item and execution objects, keeping only small SELECT-path stubs. | 76,616 | 610,014 | Passed smokes and harness. Window function SQL is unsupported. |
| 27 | Omit the fmtlib-backed `SFORMAT()` SQL function and its builders. | 72,280 | 291,688 | Passed smokes and harness. Ordinary numeric `FORMAT()` remains. |
| 28 | Add `-fno-asynchronous-unwind-tables` and `-fno-unwind-tables` without disabling C++ exceptions. | 67,136 | 188,280 | Passed smokes and harness. Reduces unwind/debugger metadata. |
| 29 | Compile embedded binlog transaction, row-event, GTID-state, and event-write entry points to no-ops and remove `rpl_record.cc`. | 66,312 | 144,570 | Passed smokes and harness. Safe first cut at no-binlog core. |
| 30 | Disable available plugin CMake flags for small built-ins. | 66,008 | 109,200 | Rejected and superseded. Large type plugins still remained mandatory. |
| 31 | Remove `sql_help.cc` and link unsupported-command shims for help execution and prepare. | 65,824 | 183,200 | Passed current smokes. SQL `HELP` is unsupported. |
| 32 | Omit `mi_check.c` and compile MyISAM check, repair, optimize, key-cache, preload, and auto-repair paths as unsupported. | 64,184 | 116,606 | Passed smokes and harness. MyISAM still served inherited disk temp tables at this point. |
| 33 | Keep public prepared API symbols but replace implementation with unsupported stubs and drop linked `mysql_stmt_*` roots. | 60,336 | 3,374 | Passed smokes and harness. Useful as a size floor, likely not a final PDO-style default. |
| 34 | Guard embedded binlog open/recovery and annotated-row helpers, then remove `gtid_index.cc`, `log_event.cc`, and `rpl_injector.cc`. | 58,880 | 240,220 | Passed smokes and harness. Keeps only tiny helpers still required outside binlog execution. |
| 35 | Replace MariaDB's generated locale table with an `en_US`-only embedded stub. | 58,552 | 289,938 | Passed smokes and harness. Non-`en_US` locale names reject. |
| 36 | Omit the `JSON_SCHEMA_VALID()` builder and `json_schema.cc` while retaining ordinary JSON functions. | 44,912 | 345,732 | Passed smokes and harness. Superseded later by broader JSON removals. |
| 37 | Remove `sql_type_geom.cc` and link minimal generic GEOMETRY metadata stubs. | 44,760 | 369,718 | Passed smokes and harness. GEOMETRY/SPATIAL DDL still rejects. |
| 38 | Omit dynamic-column SQL item execution and replace `ma_dyncol.c` with fail-closed C API stubs. | 42,464 | 176,088 | Passed smokes and harness. Dynamic-column SQL and helper APIs are unsupported. |
| 39 | Remove native builders and item implementations for server utility functions such as `BENCHMARK()`, `GET_LOCK()`, `LOAD_FILE()`, wait helpers, `SLEEP()`, and `UUID_SHORT()`. | 37,576 | 228,044 | Passed smokes and harness. Retained utility functions such as `VERSION()` remain. |
| 40 | Omit long system-variable help/comment strings while retaining names, values, defaults, validation, and `SHOW VARIABLES`. | 36,472 | 53,200 | Passed smokes and harness. Variable comments become empty. |
| 41 | Remove UDF lookup, execution, DDL runtime, and `sql_udf.cc.o`. | 36,208 | 115,930 | Passed smokes and harness. UDF support is explicitly absent. |
| 42 | Remove `spatial.cc` WKB/WKT implementation and link small remaining `Geometry` symbol stubs. | 35,872 | 140,742 | Passed smokes and harness. GEOMETRY parsing and rejection paths remain. |
| 43 | Replace full EXPLAIN, ANALYZE, and SHOW EXPLAIN plan-output runtime with an unsupported stub. | 35,136 | 221,264 | Passed smokes and harness. Ordinary optimizer bookkeeping remains. |
| 44 | Remove Oracle compatibility function aliases, Oracle native function hash, and Oracle schema routing. | 34,376 | 318,034 | Passed smokes and harness. Oracle compatibility surface shrinks further. |
| 45 | Remove CSV and MRG_MyISAM from the minsize plugin list and hide MyISAM from user DDL while retaining it internally. | 33,440 | 252,074 | Passed smokes and harness. MyISAM remained for disk temp tables at this point. |
| 46 | Replace persistent `mysql.*` statistics and JSON histogram storage with no-statistics embedded stubs. | 33,072 | 156,716 | Passed smokes and harness. Handler row estimates remain for planning. |
| 47 | Set CMake C/C++ visibility presets to hidden and keep explicit `MYLITE_API` exports. | 32,360 | 29,858 | Passed smokes and harness. Final shared-library packaging still needs deliberate export policy. |
| 48 | Omit MyISAM full-text sources, startup, sysvars, and full-text key update paths. | 29,936 | 86,788 | Passed smokes and harness. Non-full-text MyISAM temp-table use remained. |
| 49 | Reject the `JSON` type alias and parser-backed JSON aggregates, and omit JSON type handlers. | 29,592 | 413,494 | Passed smokes and harness. `LONGTEXT` remains available. |
| 50 | Compile out runtime `dlopen()` plugin loading and replace the dynamic plugin service bridge with a minimal placeholder. | 26,192 | 49,278 | Passed smokes and harness. `have_dynamic_loading=NO`; `libcrypto.so.3` still remained. |
| 51 | Omit OpenSSL-backed SQL crypto/password functions such as AES, MD5, SHA, PASSWORD, OLD_PASSWORD, and RANDOM_BYTES. | 26,016 | 137,974 | Passed smokes and harness. `libcrypto.so.3` still remained due to other roots. |
| 52 | Omit inherited remote client fallback, option defaults, client plugin loading, connect attributes, and OS username fallback. | 25,696 | 10,620 | Passed smokes and harness. Local embedded `mysql_real_connect()` still worked then. |
| 53 | Remove query cache objects and link a disabled `Query_cache` shim. | 23,512 | 73,154 | Passed smokes and harness. `have_query_cache=NO`. |
| 54 | Omit MyISAM RTREE and spatial-key sources and stop advertising RTREE key support. | 21,128 | 43,796 | Passed smokes and harness. Ordinary MyISAM disk temp tables remained. |
| 55 | Replace SQL statement digest normalization with no-op embedded stubs and omit the parser digest token table. | 18,736 | 53,842 | Passed smokes and harness. `max_digest_length=0`. |
| 56 | Replace table maintenance, key-cache assignment, and index-preload execution with unsupported embedded stubs. | 17,880 | 32,192 | Passed smokes and harness. Prepared admin metadata remains. |
| 57 | Fail unknown stored-function lookup without constructing `Item_func_sp`. | 15,720 | 43,324 | Passed smokes and harness. Broader stored-program runtime remained until a later cut. |
| 58 | Disable general and slow query logging and prune unreachable query-log handler bodies. | 15,568 | 48,310 | Passed smokes and harness. Error logging and explicit unsupported diagnostics remain. |
| 59 | Remove PCRE-backed regex execution and native regex function builders, and stop linking PCRE2 into the embedded merge list. | 15,552 | 77,814 | Passed smokes and harness. PCRE2 runtime dependency removed. |
| 60 | Remove static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` result producers. | 15,368 | 35,166 | Passed smokes and harness. Server-info help surfaces are absent. |
| 61 | Omit status publication arrays and the dynamic status-variable registry. | 14,760 | 37,644 | Passed smokes and harness. `SHOW STATUS` and status Information Schema tables return empty result sets. |
| 62 | Remove SQL sequence plugin registration, `sql_sequence.cc`, and `ha_sequence.cc`, then link unsupported stubs. | 14,376 | 217,508 | Passed smokes and harness. Sequence syntax remains but execution is unsupported or missing. |
| 63 | Reject `SHOW CREATE` variants and compile out heavy CREATE-statement formatting bodies. | 14,352 | 100,150 | Passed smokes and harness. Useful as a size-floor introspection tradeoff. |
| 64 | Replace file-backed trigger loader and runtime with inert embedded no-trigger stubs. | 13,776 | 71,494 | Passed smokes and harness. Trigger DDL remains rejected. |
| 65 | Replace file-backed view loader and runtime with embedded-disabled stubs while preserving derived-table and CTE helpers. | 13,296 | 38,128 | Passed smokes and harness. View DDL remains rejected. |
| 66 | Omit zlib-backed SQL compression, compressed columns, compressed protocol reporting, and compressed-binlog helpers. | 12,832 | 71,054 | Passed smokes and harness. `libz.so.1` left the runtime dependency set. |
| 67 | Remove `LOAD DATA` and `LOAD XML` execution object while keeping ordinary insert paths. | 11,800 | 41,244 | Passed smokes and harness. Server-file import is unsupported. |
| 68 | Omit PL/SQL cursor attribute item runtime. | 11,784 | 69,394 | Passed smokes and harness. Oracle mode and stored routines were already unsupported. |
| 69 | Remove VIO TLS transport from the aggressive embedded profile. | 11,528 | 21,898 | Passed smokes and harness. `libssl.so.3` left the runtime dependency set. |
| 70 | Replace `JSON_TABLE` table-function execution with an unsupported embedded stub. | 10,224 | 115,620 | Passed smokes and harness. Ordinary JSON scalar functions were still retained then. |
| 71 | Omit disabled binlog, GTID-binlog, relay-log, and replication system-variable registration. | 10,224 | 89,776 | Passed smokes and harness. Common harmless compatibility variables remain. |
| 72 | Omit network password-auth plugin descriptors and plugin VIO handshake helpers. | 8,656 | 19,994 | Passed smokes and harness. Local in-process open path remains. |
| 73 | Replace `libmylite`'s owned `MYSQL *` connection with an opaque direct embedded session. | 7,744 | -6,394 | Passed smokes and harness. Linked smoke win is small, but hidden-entry shared probe dropped by 231,440 bytes. |
| 74 | Omit inherited server-side encryption hooks for binlogs, relay logs, and encrypted temporary IO caches. | 7,560 | 17,502 | Passed smokes and harness. `libcrypto.so.3` still remained at that point. |
| 75 | Replace full XA transaction implementation with embedded-disabled stubs. | 7,344 | 132,588 | Passed smokes and harness. XA commands report embedded-disabled diagnostics. |
| 76 | Omit the retained `VECTOR` type handler after vector functions and indexes are already unsupported. | 7,160 | 143,372 | Passed smokes and harness. `VECTOR` columns become unknown. |
| 77 | Omit hardcoded `my_long_options[]` help strings while retaining option parser metadata. | 7,152 | 7,096 | Passed smokes and harness. Option help prose is empty. |
| 78 | Replace optimizer trace diagnostics with an inert embedded stub while preserving shared JSON writer helpers. | 6,688 | 27,752 | Passed smokes and harness. Optimizer trace output is absent. |
| 79 | Reject SQL `HANDLER` commands and replace `sql_handler.cc` with tiny embedded stubs. | 6,648 | 20,550 | Passed smokes and harness. Generic storage-engine handler abstraction remains. |
| 80 | Omit legacy DES SQL functions and DES key-file administration. | 6,624 | 41,090 | Passed smokes and harness. `libcrypto.so.3` still remained. |
| 81 | Omit SQL `GET DIAGNOSTICS`, `SIGNAL`, and `RESIGNAL` runtime while keeping internal diagnostics. | 6,560 | 115,754 | Passed smokes and harness. MyLite C API diagnostics remain. |
| 82 | Make routine Information Schema tables and routine status commands return empty results without scanning `mysql.proc`. | 6,400 | 24,892 | Passed smokes and harness. Safe while routines are unsupported. |
| 83 | Omit the inherited mmap-backed `tc.log` transaction coordinator implementation. | 6,120 | 17,500 | Passed smokes and harness. Leaves inert status variables. |
| 84 | Reject SQL `MATCH ... AGAINST` and compile out `Item_func_match` method bodies. | 5,904 | 29,592 | Passed smokes and harness. MyISAM full-text was already omitted. |
| 85 | Replace row-replication type-conversion utilities with fail-closed embedded stubs. | 5,824 | 17,996 | Passed smokes and harness. Replication conversion is unsupported. |
| 86 | Replace table-backed named time-zone loading with a stub that keeps `SYSTEM` and numeric offsets. | 5,800 | 21,910 | Passed smokes and harness. Named zones such as `Europe/Prague` reject. |
| 87 | Omit the OpenSSL-backed `KDF()` SQL function. | 5,136 | 28,816 | Passed smokes and harness. `libcrypto.so.3` still remained. |
| 88 | Bypass embedded option-file loading after the required `--no-defaults` startup argument. | 5,128 | -64 | Passed smokes and harness. Linked artifacts shrink while the merged archive grows by 64 bytes. |
| 89 | Add lld `-O2` to executable, module, and shared linker flags. | 5,120 | 0 | Passed smokes and harness. Keeps build-id and RELRO. |
| 90 | Reject `SELECT ... INTO OUTFILE` and `DUMPFILE`, and compile out host-file writer bodies. | 3,920 | 19,526 | Passed smokes and harness. `SELECT ... INTO` variables remain. |
| 91 | Replace the `mysql.servers` foreign-server metadata cache with no-op embedded stubs. | 3,632 | 30,750 | Passed smokes and harness. `CREATE SERVER` surfaces remain rejected. |
| 92 | Replace external backup stage, backup lock, and backup DDL logging with embedded stubs. | 3,400 | 10,586 | Passed smokes and harness. Backup statements are unsupported. |
| 93 | Omit legacy MySQL 5.0 utf8mb3 and ucs2 compatibility collations. | 3,272 | 8,680 | Passed smokes and harness. Explicit use fails as unknown collation. |
| 94 | Reject `SHOW PROCESSLIST` and make `INFORMATION_SCHEMA.PROCESSLIST` return empty rows. | 3,048 | 41,234 | Passed smokes and harness. Daemon process-list reporting is absent. |
| 95 | Omit legacy `ENCRYPT()` and stop linking `${LIBCRYPT}` for that function. | 2,864 | 18,408 | Passed smokes and harness. `libcrypt.so.1` left the runtime dependency set. |
| 96 | Omit system-versioned table predicate item runtime and reject temporal table metadata. | 2,536 | 114,912 | Passed smokes and harness. Tiny retained methods live in already-linked code. |
| 97 | Remove option-table rows for disabled binlog, replication, and dynamic plugin-loading options. | 2,512 | 3,736 | Passed smokes and harness. Removes knobs for unavailable server subsystems. |
| 98 | Run `strip --strip-unneeded --strip-section-headers` on copied release artifacts. | 2,360 | n/a | PHP-shaped shared-probe measurement. Opt-in packaging floor. Runtime probe passed, but post-link inspection and debugging get worse. |
| 99 | Remove SQL-language `PREPARE`, `EXECUTE`, `EXECUTE IMMEDIATE`, and `DEALLOCATE PREPARE` command entry points. | 1,856 | 11,374 | Passed smokes and harness. Public MyLite prepared statements still existed then. |
| 100 | Skip inherited `#binlog_cache_files` directory setup in the embedded no-binlog profile. | 1,528 | 88 | Passed smokes and harness. Also removes a daemon-style startup filesystem step. |
| 101 | Route `mylite_exec()` and `mylite_warning()` through a MyLite-owned direct embedded result adapter. | 1,520 | -2,590 | Passed smokes and harness. Groundwork for removing inherited client C API roots. |
| 102 | Omit vector SQL functions, MHNSW sources, and the mandatory `mhnsw` plugin reference. | 1,152 | 230,182 | Passed current smokes. Vector functions and MHNSW indexes are unsupported. |
| 103 | Disable optional built-ins such as `sequence`, `thread_pool_info`, `user_variables`, and `userstat`. | 1,016 | 131,980 | Passed current smokes. Small linked win, clearer plugin surface. |
| 104 | Replace replication filter behavior with a minimal permissive embedded stub. | 816 | 35,208 | Passed smokes and harness. Mostly archive cleanup. |
| 105 | Replace full `rpl_gtid.cc` with a tiny no-binlog lifecycle stub. | 600 | 128,438 | Passed smokes and harness. Archive win, tiny linked win. |
| 106 | Skip `mysql_bin_log` instrumentation setup, pthread-object init, and cleanup in the embedded no-binlog profile. | 448 | 288 | Passed smokes and harness. Embedded startup cleanup, not a material size lever. |
| 107 | Move retained SQL string-rendering helper into a minsize stub and omit `log_event_server.cc.o`. | 424 | 299,846 | Passed smokes and harness. Large archive win, tiny linked win. |
| 108 | Replace generic `SELECT ... PROCEDURE` runtime with an unsupported embedded stub. | 336 | 120,998 | Passed smokes and harness. `PROCEDURE ANALYSE()` was already unsupported. |
| 109 | Replace inherited `tpool_wait_begin()` and `tpool_wait_end()` runtime with no-op embedded hooks. | 304 | -1,554 | Passed smokes and harness. Valid embedded cleanup with negligible size effect. |
| 110 | Relink runtime artifacts with RELRO disabled. | 208 | n/a | Rejected. A 208-byte saving was not worth dropping RELRO hardening. |
| 111 | Switch aggressive minsize compile flags from `-Os` to `-Oz`. | 128 | 0 | Passed smokes and harness. Marginal linked-runtime win, not meaningful pruning. |
| 112 | Replace full event parser data validation with a minimal parser-allocation stub. | 88 | 11,856 | Passed smokes and harness. Event DDL remains rejected. |
| 113 | Enable function/data sections and section GC before removing executable exports. | 88 | -4,899,920 | Rejected and superseded. Linked size barely moved and archive size grew. |
| 114 | Replace proxy protocol network-listener support with disabled embedded stubs. | 80 | 8,012 | Passed smokes and harness. Network listener support is absent. |
| 115 | Apply `strip --strip-unneeded` and `ranlib` to `libmariadbd.a` as part of the minsize build. | 0 | 1,337,440 | Passed current smokes. Static archive distribution win; linked smoke unchanged. |
| 116 | Disable statement profiling and omit full profiling classes. | 0 | 166,334 | Passed current smokes. Archive win only in this measurement. |
| 117 | Remove `sql_analyse.cc` and link an unsupported `proc_analyse_init()` shim. | 0 | 154,008 | Passed current smokes. Archive win only in this measurement. |
| 118 | Toggle all tested Performance Schema disable switches in the existing minsize profile. | 0 | 0 | Rejected. No measured size effect. |
| 119 | Add only `-fno-asynchronous-unwind-tables` to C and C++ flags. | 0 | 0 | Rejected. Smokes passed, but artifact sizes and unwind sections were unchanged. |
| 120 | Build with GCC non-fat LTO objects and use GNU bfd because lld cannot consume them. | 0 | -917,856 | Rejected. Saved no stripped bytes and grew archive and unstripped runtime artifacts. |
| 121 | Relink runtime artifacts with smaller max page size and no separate code segment. | 0 | n/a | Rejected. Segment-alignment relink did not reduce stripped file size. |
| 122 | Add an audit script that relinks a PHP-extension-shaped shared-object probe and reports exports, dependencies, map contributors, and stripped sizes. | 0 | n/a | Measurement tool, not a reduction attempt. Audit-only. Current corrected probe is 3,861,728 bytes stripped and 3,859,368 bytes sectionless. |
| 123 | Omit command-level binlog replay and replication SQL sources while keeping transaction participant roots. | -512 | 23,172 | Passed smokes and harness. Linked-runtime growth is smoke-test noise; mainly archive cleanup. |
| 124 | Replace retained OpenSSL-backed MD5/SHA-1 wrappers and omit unused OpenSSL wrapper objects and cleanup paths. | -680 | 39,888 | Passed smokes and harness. Linked smoke grew, but `libcrypto.so.3` was removed; vendored dependency saving is 4,597,928 bytes before compression. |
| 125 | Shrink the process-global `all_charsets` pointer registry from 4096 entries to 1152 entries. | -960 | -24 | Opt-in memory-footprint attempt. `llvm-size` dropped by 47,180 bytes through `.bss`, but stripped bundle size grew. |
| 126 | Add explicit unsupported diagnostics for server user, role, grant, revoke, and related account SQL. | -1,712 | -576 | Passed smokes and harness. Correctness change, not a size win. |
| 127 | Disable MariaDB's `SECURITY_HARDENED` CMake option. | -101,968 | -479,460 | Rejected. Smokes and harness passed, but both archive and linked runtime grew while hardening weakened. |
| 128 | Teach CMake to accept `WITH_SSL=OFF` and try building the embedded minsize profile without SSL. | n/a | n/a | Unbuilt attempt. Rejected as not narrow. Compilation failed in VIO and `mysys_ssl` digest/AES code. Later staged SSL and crypto removals handled the viable pieces. |
| 129 | Compile retained C++ sources without RTTI. | n/a | n/a | Failed build. Rejected. Retained SQL headers use `dynamic_cast`, so the build failed. |
| 130 | Compile all C++ targets without exceptions. | n/a | n/a | Failed build. Rejected as a global flag. MariaDB thread-pool code catches `std::system_error`; the later `sql_embedded`-only profile was the viable narrower cut. |

## Measurement-only findings

| Finding | Data point | What it means |
| --- | --- | --- |
| First reproducible minsize baseline | `libmariadbd.a` was 44,134,820 bytes with 570 archive objects and no dynamic plugin artifacts. | The branch started with a measurable embedded MariaDB baseline whose built-in plugin set still included Aria, binlog, CSV, HEAP, MHNSW, MyISAM, MyISAMMRG, sequence, SQL sequence, type, and user plugins. |
| Early static footprint concentration | Before the first reductions, `libsql_embedded.a` was 32,826,258 bytes, `libstrings.a` was 4,839,668 bytes, `libtype_inet_embedded.a` was 2,384,608 bytes, and `libtype_uuid_embedded.a` was 1,066,660 bytes. | The largest opportunities were in SQL/parser/function, charset/collation, and optional type-plugin code, not in MyLite's first-party wrapper. |
| Binlog removal boundary | The initially identified binlog/replay object cluster was roughly 512 KiB of archive code before compression, but `mysql_bin_log` and `binlog_tp` stayed tied into transaction-manager paths. | Safe work had to proceed through section GC, command-surface rejection, and small no-binlog stubs instead of deleting `log.cc` wholesale. |
| Minimal linked consumer proxy | Before direct sessions, a minimal executable that only called `mylite_open()` and `mylite_close()` was 4,344,120 bytes stripped, 107,984 bytes smaller than the stripped open-close smoke at that point. | The open-close smoke was a conservative linked-payload proxy; it included about 0.10 MiB of harness overhead before later direct-session work. |
| Early PHP-shaped shared probe | After the SQL-exceptions cut, a one-export shared-object probe was 3,886,048 bytes stripped and depended only on `libstdc++`, `libm`, `libgcc_s`, and `libc`. | Extension-shaped artifacts were already smaller than the executable smoke; later direct-session and audit work made this the better packaging signal. |
| Research-stack bundle audit | The corrected PHP-shaped shared probe in that research stack is 3,861,728 bytes stripped and 3,859,368 bytes without section headers, with one export, no unused direct dynamic dependencies, and no checked embedded-client C API roots. | This is useful historical GCC packaging evidence, but it is not the current default-profile artifact; rerun candidates against the current baseline before accepting them. |
| Vendored dynamic dependencies | Bundling the current dynamic dependencies would add 5,081,640 bytes, or 4.85 MiB, before compression. | These libraries are outside `libmariadbd.a`; dependency bundling is a packaging decision, not a static archive saving. |
| Remaining binlog shell | After lld `-O2`, the residual shell was about 4.4 KiB of `mysql_bin_log` BSS, 160 bytes of `binlog_tp`, and small no-op methods/vtables. | Further binlog-shell removal is not worth near-term size work unless MyLite undertakes a broader transaction-log class-layout refactor. |
