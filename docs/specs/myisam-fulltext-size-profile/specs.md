# MyISAM Full-Text Size Profile

## Problem

The default aggressive minsize profile still keeps MyISAM because MariaDB uses
it for inherited disk temporary tables. User-created `ENGINE=MyISAM` tables are
already hidden, and MyLite tables already reject `FULLTEXT` indexes. The
remaining MyISAM build still includes full-text index implementation code,
startup stopword initialization, and full-text system variables that are not
needed for ordinary MyISAM temporary-table spill.

Current measured MyISAM full-text component object sizes from
`build/mariadb-minsize-myisam-admin/storage/myisam/CMakeFiles/myisam_embedded.dir`:

| Object | Bytes |
| --- | ---: |
| `ft_static.c.o` | 25,568 |
| `ft_boolean_search.c.o` | 16,240 |
| `ft_update.c.o` | 9,656 |
| `ft_parser.c.o` | 9,520 |
| `ft_nlq_search.c.o` | 9,024 |
| `ft_stopwords.c.o` | 6,432 |
| `ft_myisam.c.o` | 1,632 |
| Total | 78,072 |

The linked open/close smoke also still contains live symbols such as
`ft_init_stopwords`, `ft_boolean_check_syntax_string`, `ft_init_search`,
`_mi_ft_add`, `_mi_ft_del`, `_mi_ft_update`, `_ft_make_key`, and
`ha_myisam::ft_read`.

## Source Findings

MariaDB source references are from imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `storage/myisam/CMakeLists.txt` includes `ft_boolean_search.c`,
  `ft_nlq_search.c`, `ft_parser.c`, `ft_static.c`, `ft_myisam.c`,
  `ft_stopwords.c`, and `ft_update.c` in the mandatory MyISAM plugin source
  list.
- `storage/myisam/ha_myisam.cc` advertises `HA_CAN_FULLTEXT`, copies
  `HA_FULLTEXT_legacy` into MyISAM key definitions, and routes full-text reads
  through `ha_myisam::ft_init()`, `ft_init_ext()`, and `ft_read()`.
- `storage/myisam/mi_write.c`, `mi_update.c`, and `mi_delete.c` call
  `_mi_ft_add`, `_mi_ft_del`, `_mi_ft_update`, and `_mi_ft_cmp` for MyISAM
  full-text keys.
- `storage/myisam/mi_create.c`, `mi_open.c`, `mi_key.c`, `mi_packrec.c`,
  `mi_search.c`, and `sort.c` contain full-text key layout branches. Normal
  BTREE temporary tables do not set `HA_KEY_ALG_FULLTEXT`, but these branches
  can keep removed full-text helpers referenced at link time unless they are
  guarded or otherwise made unreachable.
- `sql/mysqld.cc` initializes full-text stopwords with `ft_init_stopwords()`
  and validates `ft_boolean_syntax` during startup.
- `sql/sys_vars.cc` registers `ft_boolean_syntax`, `ft_max_word_len`,
  `ft_min_word_len`, `ft_query_expansion_limit`, and `ft_stopword_file`.
- Existing MyLite storage smoke coverage already rejects `FULLTEXT KEY` on
  MyLite tables, and the legacy-storage-engine profile already rejects explicit
  user `ENGINE=MyISAM`.

## Scope

Add a `MYLITE_DISABLE_MYISAM_FULLTEXT` minsize option that:

- omits MyISAM full-text implementation sources from the embedded MyISAM build,
- stops advertising full-text support from the MyISAM handlerton,
- rejects accidental MyISAM full-text key creation in the disabled profile,
- compiles out MyISAM write/update/delete full-text branches that would require
  removed helpers,
- skips MyISAM full-text startup stopword initialization and system variables,
  and
- preserves ordinary MyISAM disk temporary-table create/open/read/write/delete
  paths.

## Non-Goals

This slice does not remove MyISAM itself, MyISAM BTREE/RTREE code, or the
handlerton needed by MariaDB's inherited disk temporary-table path.

This slice does not remove the SQL parser's `MATCH ... AGAINST` grammar or
general SQL-layer full-text error paths. Without an engine that advertises
`HA_CAN_FULLTEXT`, those paths should continue to fail through MariaDB's
existing unsupported-full-text diagnostics.

This slice does not change MyLite table behavior. MyLite full-text indexes
remain unsupported.

## Binary-Size Impact

The static archive should drop at least the removed full-text object code,
roughly 75 to 80 KiB before archive metadata effects. The linked runtime impact
may be larger than the raw object total because startup and system-variable
roots currently keep full-text parser and stopword code live.

## DDL Metadata Routing Impact

No MyLite catalog-format change. User `FULLTEXT` DDL for MyLite tables is
already rejected before table-definition persistence. If a user somehow targets
MyISAM full-text in this profile, the hidden MyISAM engine or MyISAM key
creation path must reject it rather than creating `.MYD` or `.MYI` files.

## Single-File And Embedded-Lifecycle Implications

Removing MyISAM full-text support reduces inherited non-MyLite table behavior
inside the embedded runtime. It does not change the current temporary spill
allowance: MyISAM remains present only for inherited temporary tables under
MyLite-controlled runtime directories.

Skipping stopword initialization also removes startup work and avoids retaining
`ft_stopword_file` handling in the embedded profile.

## Public API Or File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## License, Trademark, And Dependency Impact

No new dependency or licensing change.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-myisam-fulltext MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-myisam-fulltext MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-myisam-fulltext MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
git diff --check
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh tools/run-compatibility-test-harness.sh
```

Measure:

- `libmysqld/libmariadbd.a` bytes and object count,
- `storage/myisam/libmyisam_embedded.a` bytes and object count,
- stripped `mylite-open-close-smoke` bytes,
- absence of `ft_*.c.o` members from the merged archive, and
- absence of live `ft_init_search`, `_mi_ft_add`, `_mi_ft_del`,
  `_mi_ft_update`, `_ft_make_key`, and `ft_init_stopwords` symbols from the
  linked smoke.

## Acceptance Criteria

- Default minsize build links without MyISAM full-text source objects.
- Open/close smoke and full compatibility harness pass.
- User `ENGINE=MyISAM` remains explicitly unavailable.
- MyLite `FULLTEXT` index DDL remains rejected.
- Disk temporary-table execution still works for schema-table metadata and
  compatibility smokes.
- Size deltas are recorded in `docs/research/production-size-analysis.md`.

## Risks

- MariaDB full-text SQL-layer code remains compiled. That is acceptable for
  this bounded slice, but it means `MATCH ... AGAINST` parser and planner code
  may still contribute size until a separate SQL-layer full-text slice proves it
  can be removed safely.
- A hidden MyISAM temp-table path should not request full-text indexes. If it
  does, the disabled profile must fail clearly rather than linking full-text
  implementation code back in.
