# BuddyPress Component Schema Fixture

## Problem Statement

MyLite application-schema coverage imports WordPress core installer and
multisite fixtures, but it does not yet cover large plugin-owned application
tables. The fixture must be license-compatible with MyLite's GPL-2.0 MariaDB
foundation.

BuddyPress is GPLv2-or-later and creates a broad set of WordPress-prefixed
component tables for activity streams, notifications, friends, groups, private
messages, extended profiles, site tracking, invitations, and nonmember
opt-outs. This slice adds a pinned BuddyPress schema fixture and a
storage-engine smoke test for omitted-engine routing, representative rows,
indexed reads, close/reopen discovery, and sidecar gates.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). The relevant MyLite behavior is
  routed `CREATE TABLE` metadata, row storage, indexes, close/reopen discovery,
  and sidecar lifecycle under the existing MariaDB 11.8 embedded storage-smoke
  build.
- BuddyPress source is tag `14.4.0`
  (`ae347f8fbbcda21f2a652191b881315666aa6403`) from
  `buddypress/buddypress`.
- `src/readme.txt:5-6` declares BuddyPress under the GNU General Public License
  v2 or later.
- `src/bp-core/bp-core-update.php:186-194` defines the default new-install
  active components as activity, members, settings, extended profiles, and
  notifications.
- `src/bp-core/bp-core-update.php:222-226` calls `bp_core_install()` for new
  installs and then installs email content through WordPress posts/terms.
- `src/bp-core/admin/bp-core-admin-schema.php:23-74` defines the main
  `bp_core_install()` component routing and always installs activity streams,
  signups, invitations, and nonmember opt-outs before optional component tables.
- `src/bp-core/admin/bp-core-admin-schema.php:81-424` defines the notification,
  activity, friends, groups, private messaging, extended profile, and blog
  tracking component tables.
- `src/bp-core/admin/bp-core-admin-schema.php:436-466` installs the signups
  table by reusing WordPress core multisite global schema, which is already
  covered by the WordPress multisite fixture.
- `src/bp-core/admin/bp-core-admin-schema.php:565-626` defines the invitations
  and nonmember opt-out tables.
- `src/bp-members/classes/class-bp-members-component.php:350-354` names
  BuddyPress global members tables as `bp_invitations`, `bp_activity`,
  `bp_optouts`, and WordPress's global `signups`.

## Proposed Design

Add `packages/libmylite/tests/fixtures/buddypress-14.4.0-component-schema.sql`
with deterministic substitutions:

- BuddyPress table prefix becomes the default WordPress `wp_` prefix.
- `$wpdb->get_charset_collate()` becomes
  `DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci`.
- Meta-key prefix indexes keep BuddyPress's `191` length.
- The fixture includes all BuddyPress component-owned tables from
  `bp_core_install()` for a full-component install.
- The WordPress core `wp_signups` table is intentionally omitted because the
  WordPress multisite fixture already covers it from the WordPress source.
- BuddyPress email content installed into WordPress posts/terms is out of
  scope for this plugin-table fixture.

Add `packages/libmylite/tests/fixtures/buddypress-14.4.0-component-seed.sql`
with deterministic representative rows for all fixture tables.

Extend the storage-engine smoke test with a BuddyPress fixture case that:

- creates a `buddypress_install` schema with WordPress-style utf8mb4 defaults,
- imports the schema fixture,
- verifies all fixture tables are cataloged with requested engine `DEFAULT`
  and effective engine `MYLITE`,
- imports representative rows and checks forced-index reads across activity,
  notifications, friends, groups, messages, xProfile, blog tracking,
  invitations, and opt-outs,
- verifies table collation metadata for representative tables,
- verifies close/reopen persistence and no durable MariaDB sidecars.

## Affected MariaDB Subsystems

- SQL parser and `CREATE TABLE` execution for WordPress/BuddyPress-style DDL.
- Handler routing for omitted engine requests under the static MyLite storage
  engine.
- Table-definition catalog storage and discovery after close/reopen.
- Row, autoincrement, primary-key, secondary-index, prefix-index, and
  composite-query storage paths.

No upstream MariaDB source changes are expected.

## MySQL/MariaDB Compatibility Impact

The fixture proves representative BuddyPress component tables can be accepted
by MyLite's MariaDB-derived SQL layer and routed to MyLite storage. It does not
claim full BuddyPress runtime compatibility, WordPress `dbDelta()`
compatibility, plugin activation hooks, email content setup, or exhaustive
BuddyPress application behavior.

## DDL Metadata Routing Impact

The slice exercises omitted-engine DDL routed to MyLite. Catalog metadata must
preserve table definitions, keys, character set, collation, requested engine
`DEFAULT`, and effective engine `MYLITE` without durable `.frm` files or
engine-specific sidecars.

## Single-File And Embedded-Lifecycle Implications

All durable table metadata, row data, index entries, and autoincrement state
must remain inside the primary `.mylite` file. The test keeps existing
storage-smoke lifecycle gates for close/reopen and forbidden durable sidecars.

## Public API Or File-Format Impact

There is no public API change and no intentional file-format change. The slice
uses existing DDL, row, index, and catalog records.

## Storage-Engine Routing Impact

The fixture uses omitted-engine `CREATE TABLE` statements. They should record
requested engine `DEFAULT` and route to effective engine `MYLITE`, matching
WordPress installer fixtures.

## Wire-Protocol Or Integration-Package Impact

None. This is embedded storage-smoke coverage only.

## Binary-Size Impact

None expected. The slice adds tests, docs, and SQL fixtures only.

## License Or Dependency Impact

No dependency is added. BuddyPress 14.4.0 declares GNU General Public License
v2 or later in `src/readme.txt:5-6`, so the derived test fixture is compatible
with this GPL-2.0 project.

## Test And Verification Plan

- `cmake --build --preset storage-smoke-dev --target format`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure -R libmylite.embedded-storage-engine`
- `tools/mylite-compat-harness report application-schema`
- `cmake --build --preset dev && ctest --preset dev --output-on-failure`
- `cmake --build --preset embedded-dev && ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The BuddyPress fixture imports under `storage-smoke-dev`.
- All fixture table metadata is catalog-backed and survives close/reopen.
- Representative keyed row reads pass before and after close/reopen.
- The application-schema harness reports the new coverage.
- Documentation describes BuddyPress schema coverage without claiming full
  plugin runtime support.
- No durable MariaDB sidecars are introduced.

## Risks And Unresolved Questions

- BuddyPress's installer normally runs through WordPress `dbDelta()`. This
  slice imports the normalized DDL directly and does not execute `dbDelta()`.
- The fixture includes all BuddyPress component-owned table DDL, not the
  WordPress core `wp_signups` table or BuddyPress email post/term content.
- Full BuddyPress runtime compatibility still needs plugin activation,
  component setup, `dbDelta()` behavior, email content, and application-level
  CRUD tests.
