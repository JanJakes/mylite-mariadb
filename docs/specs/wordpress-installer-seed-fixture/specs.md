# WordPress Installer Seed Fixture

## Problem

MyLite imports the WordPress 6.9.4 single-site installer DDL fixture, but it
does not yet prove the initial installer data path. WordPress installation does
more than create tables: it inserts default options, creates the first
administrator, seeds the default category, first post, first comment, first
page, post metadata, and user metadata.

This slice adds a deterministic SQL seed fixture derived from the WordPress
installer data paths and proves the seeded rows survive close/reopen through
MyLite storage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

WordPress source: `WordPress/WordPress` tag `6.9.4`
(`97b7f62adb5d8864c3fac554bc7182d9fd754a41`).

- `wp-includes/version.php:17-26` identifies WordPress `6.9.4` and database
  revision `60717`.
- `wp-admin/includes/upgrade.php:47-145` runs `make_db_current_silent()`,
  `populate_options()`, `populate_roles()`, installer option updates, default
  user creation, role assignment, and `wp_install_defaults()`.
- `wp-admin/includes/schema.php:362-622` defines the default option population
  path and the bulk `wp_options` insert pattern.
- `wp-admin/includes/upgrade.php:178-470` inserts the default category, term
  taxonomy, first post, term relationship, first comment, first page, post
  metadata, widget options, and welcome-panel user metadata.
- The current MyLite fixture executor split SQL on every semicolon. WordPress
  serialized option and usermeta values contain semicolons, so the executor
  must split only on statement terminators outside quoted strings.

Reference URLs:

- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-includes/version.php>
- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-admin/includes/schema.php>
- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-admin/includes/upgrade.php>

## Design

Add a second versioned fixture beside the schema fixture:

- `wordpress-6.9.4-single-site-seed.sql`.

The fixture is deterministic SQL, not a PHP runtime:

- table prefix is `wp_`;
- site URL is `https://example.test`;
- installer title/user/email values are stable test values;
- timestamps are fixed;
- WordPress database revision is `60717`;
- serialized option and usermeta values are precomputed.

Extend the storage-smoke installer fixture test to:

1. import the existing schema fixture;
2. import the seed fixture through `mylite_exec()`;
3. assert representative default options, the default role payload,
   administrator rows, usermeta, taxonomy rows, first post, first comment,
   first page metadata, and serialized widget option payloads;
4. close and reopen the `.mylite` file;
5. repeat the seed-data assertions without rehydrating durable sidecars.

The fixture executor should be tightened in test code so semicolons inside
single-quoted or double-quoted SQL strings are not treated as statement
separators. This is test infrastructure only; it does not change MyLite runtime
semantics.

## Supported Scope

- Representative WordPress 6.9.4 single-site installer data.
- `wp_options`, `wp_users`, `wp_usermeta`, `wp_terms`, `wp_term_taxonomy`,
  `wp_term_relationships`, `wp_comments`, `wp_posts`, and `wp_postmeta`.
- Serialized WordPress option/usermeta payloads with semicolons inside SQL
  string literals.
- Close/reopen discovery and indexed reads over seeded rows.

## Non-Goals

- Running WordPress PHP, WP-CLI, or a full browser installer.
- Exhaustively reproducing every `populate_options()` default or role
  capability.
- Privacy-policy page generation, because WordPress derives that content from
  PHP helper classes.
- Pretty-permalink probing, rewrite flushing, cron scheduling, email
  notification, multisite, plugin tables, or upgrade paths.

## Compatibility Impact

Application-schema compatibility remains partial, but the WordPress coverage now
includes a versioned DDL fixture, representative installer seed data, broader
default-option rows, and the full default role payload. This is still not full
WordPress runtime compatibility.

## Single-File And Embedded-Lifecycle Impact

The seed fixture must keep durable state in the primary `.mylite` file. The
storage-smoke sidecar gates continue to reject durable MariaDB sidecars after
close and after reopen.

## Public API Or File-Format Impact

No public API or storage format change is intended. The fixture uses
`mylite_exec()` over existing table, index, row, and catalog behavior.

## Storage-Engine Routing Impact

No new engine mapping is added. The seed runs against the tables imported by the
omitted-engine WordPress schema fixture, which route to MyLite.

## Binary-Size Impact

No default build-profile or linked-runtime change is expected. This slice adds
test fixture data and test-only SQL splitting.

## License Or Dependency Impact

The fixture is derived from GPL-2.0-or-later WordPress installer behavior and is
compatible with this GPL-2.0 MariaDB-derived repository. No runtime dependency
is added.

## Test And Verification Plan

- Add the seed fixture.
- Extend `mylite_embedded_storage_engine_test` to import the seed after the
  schema fixture and assert representative rows before and after reopen.
- Run the application-schema compatibility group.
- Run format, tidy, diff, dev, embedded, and storage-smoke checks before commit.

## Acceptance Criteria

- The WordPress 6.9.4 single-site schema and seed fixtures import in one
  storage-smoke database.
- Semicolons inside serialized WordPress SQL string literals do not break
  fixture execution.
- Representative seeded options, default roles, user, usermeta, taxonomy, post,
  comment, and postmeta rows read back through indexed or joined access paths
  before and after reopen.
- No durable sidecars remain after close.
- Compatibility and roadmap docs distinguish this representative seed fixture
  from full WordPress runtime install support.

## Risks And Open Questions

- A static SQL seed cannot cover WordPress's dynamic PHP filters, generated
  privacy policy content, cron scheduling, rewrite probing, or mail paths.
- The seed is representative rather than exhaustive. Although default role
  payload storage is now covered, a later full runtime installer test should
  compare against an actual WordPress install database.
