# WordPress Installer Schema Fixture

## Problem

The current application-schema smoke is WordPress-shaped, but it is still
hand-written. MyLite needs a versioned fixture derived from WordPress installer
DDL before compatibility claims can move toward exact application imports.

This slice adds a pinned single-site WordPress schema fixture and proves that
MyLite can execute the installer table DDL through routed storage without
durable MariaDB sidecars.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

WordPress source: `WordPress/WordPress` tag `6.9.4`
(`97b7f62adb5d8864c3fac554bc7182d9fd754a41`).

- WordPress `wp-admin/includes/schema.php` `wp_get_db_schema()` builds the
  installer table DDL for `all`, `global`, `ms_global`, and `blog` scopes.
- For single-site installs, `wp_get_db_schema('all')` returns the single-site
  users/usermeta global tables plus the blog-specific tables. Multisite global
  tables are added only when multisite is active.
- `schema.php` uses `$wpdb` table names, `$max_index_length = 191`, and
  `$wpdb->get_charset_collate()` rather than explicit storage-engine clauses.
- MyLite storage-smoke sessions set `default_storage_engine=MYLITE` and
  `enforce_storage_engine=MYLITE`, so omitted WordPress `ENGINE` clauses route
  to the MyLite handler and are recorded as requested `DEFAULT`, effective
  `MYLITE`.
- MariaDB `sql/sql_table.cc` and `storage/mylite/ha_mylite.cc` already handle
  the covered `CREATE TABLE` path, supported integer display-width syntax,
  text families, `AUTO_INCREMENT`, composite keys, and prefix indexes.

Reference URLs:

- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-admin/includes/schema.php>
- <https://wordpress.org/documentation/article/wordpress-versions/>

## Design

Add a first-party fixture file containing the single-site
`wp_get_db_schema('all')` table DDL after deterministic installer-time
substitution:

- `$wpdb` table names become the default `wp_` prefix;
- `$max_index_length` becomes `191`;
- `$charset_collate` becomes
  `DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci`;
- no `ENGINE` clauses are added.

The fixture lives under the `libmylite` test tree and is executed by the
storage-smoke test through the public `mylite_exec()` API. The test creates a
schema with WordPress-compatible defaults, imports each fixture statement,
checks catalog metadata for all imported tables, checks representative table
collations, closes and reopens the `.mylite` file, and repeats metadata checks.

The existing WordPress-shaped smoke remains in place because it exercises DML,
joins, updates, deletes, and application row/index behavior. This slice adds an
installer-DDL fixture rather than replacing the row-behavior smoke.

## Supported Scope

- WordPress 6.9.4 single-site schema DDL.
- `wp_users`, `wp_usermeta`, and blog-specific `wp_termmeta`, `wp_terms`,
  `wp_term_taxonomy`, `wp_term_relationships`, `wp_commentmeta`,
  `wp_comments`, `wp_links`, `wp_options`, `wp_postmeta`, and `wp_posts`.
- Default-engine routing from omitted `ENGINE` clauses to MyLite.
- `utf8mb4_unicode_ci` table defaults via deterministic charset substitution.
- Catalog metadata and close/reopen discovery for the imported tables.

## Non-Goals

- Running WordPress PHP or WP-CLI.
- Seeding installer options, roles, users, or posts; these are covered by the
  separate WordPress installer seed fixture.
- Multisite global tables.
- Plugin tables, migrations, or upgrade paths.
- Broad charset/collation matrix coverage beyond the fixture default.
- Foreign-key enforcement; WordPress core tables do not declare database
  foreign keys.

## Compatibility Impact

Application-schema compatibility remains partial, but MyLite gains a versioned
WordPress installer schema fixture. The compatibility matrix should distinguish
this DDL import from full WordPress install/runtime compatibility.

## DDL Metadata Routing Impact

The fixture primarily exercises omitted-engine DDL, so imported table catalog
metadata should record requested engine `DEFAULT` and effective engine `MYLITE`.
The test should also prove the default character set and collation survive
close/reopen through MariaDB information schema.

## Single-File And Embedded-Lifecycle Impact

The fixture import must leave only the primary `.mylite` file after close.
Runtime directories remain transient MyLite-owned state and are removed on final
close.

## Public API Or File-Format Impact

No public API or storage format changes are intended. The fixture uses existing
`mylite_exec()` behavior and existing catalog records.

## Storage-Engine Routing Impact

No new engine mapping is added. The slice adds compatibility pressure to the
existing default-engine route.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

No default MariaDB build-profile or linked-runtime changes are expected. The new
fixture only affects test sources.

## License Or Dependency Impact

The fixture is derived from GPL-2.0-or-later WordPress schema DDL and remains
compatible with MyLite's GPL-2.0 MariaDB-derived repository. No new dependency
is added, and the source tag is recorded so the fixture can be refreshed
intentionally.

## Test And Verification Plan

- Add the versioned SQL fixture.
- Add a storage-smoke test that imports the fixture through `mylite_exec()`.
- Run the focused storage-engine smoke test.
- Run `tools/mylite-compat-harness report application-schema`.
- Run format, tidy, dev, embedded, and storage-smoke checks before commit.

## Acceptance Criteria

- The WordPress 6.9.4 single-site schema fixture imports without durable sidecar
  files.
- All imported tables are cataloged as requested `DEFAULT`, effective `MYLITE`.
- Representative imported tables expose `utf8mb4_unicode_ci` table collation
  before and after reopen.
- Roadmap and compatibility docs describe the fixture without claiming full
  WordPress runtime support.

## Risks And Open Questions

- The fixture is intentionally static. A later refresh workflow should compare
  future WordPress tags and record intentional differences.
- DDL import does not prove WordPress runtime queries, option seeding, multisite,
  or plugin behavior.
