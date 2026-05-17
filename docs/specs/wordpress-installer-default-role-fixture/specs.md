# WordPress Installer Default Options And Role Fixture

## Problem

The WordPress single-site seed fixture proves representative installer data and
the default role payload, but it still omits many deterministic
`populate_options()` defaults. The roadmap calls out installer defaults as
planned application-schema coverage. MyLite needs broader pressure on
`wp_options` inventory, autoload flags, serialized empty arrays, media/comment
defaults, and long serialized values without claiming a full PHP runtime
install.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

WordPress source: `WordPress/WordPress` tag `6.9.4`
(`97b7f62adb5d8864c3fac554bc7182d9fd754a41`).

- `wp-admin/includes/upgrade.php:79-80` calls `populate_options()` and
  `populate_roles()` during install.
- `wp-admin/includes/schema.php:411-566` defines the default option array,
  including site, mail, date/time, comment, media, avatar, permalink, serialized
  empty-array, update, and `6.9.0` notification defaults.
- `wp-admin/includes/schema.php:568-572` adds `initial_db_version` for
  single-site installs.
- `wp-admin/includes/schema.php:581-588` marks selected large text defaults as
  non-autoloaded.
- `wp-admin/includes/schema.php:719-734` runs the role population chain.
- `wp-admin/includes/schema.php:750-967` creates the default administrator,
  editor, author, contributor, and subscriber roles and adds the versioned
  capability set used by a single-site install.
- `wp-admin/includes/upgrade.php:82-87` overwrites the installer-provided
  blog name, admin email, public flag, and `fresh_site` option after
  `populate_options()`.
- MyLite already stores `LONGTEXT` option values and tests semicolons inside
  serialized SQL string literals through the WordPress seed fixture executor.

## Design

Broaden the deterministic `wordpress-6.9.4-single-site-seed.sql` fixture:

- keep the existing stable site/user/timestamp substitutions;
- add the full deterministic single-site `populate_options()` option-name set;
- pin dynamic values: the default theme to `twentytwentysix`, `wp_guess_url()`
  to `https://example.test`, and `time()` to `1778839200`;
- preserve `fresh_site` and installer-provided blog/admin/public updates from
  `wp_install()`;
- replace the minimal `wp_user_roles` payload with the serialized default
  single-site roles and capabilities for administrator, editor, author,
  contributor, and subscriber.

Extend the storage-smoke WordPress installer test to assert:

- the expanded option row count and option-name inventory are present;
- non-autoloaded defaults keep the expected `off` autoload flag;
- selected defaults from the full option array keep their expected values;
- `default_role` remains `subscriber`;
- the role payload is a long serialized option containing all five role names
  and representative administrator capabilities;
- the existing user role assignment and installer content still read back
  before and after close/reopen.

## Supported Scope

- Full deterministic WordPress 6.9.4 single-site `populate_options()` default
  option-name inventory.
- Representative value assertions across the default option array.
- Full default role-name set and capability payload as a serialized
  `wp_user_roles` option.
- Indexed `wp_options` reads and long serialized `LONGTEXT` persistence across
  close/reopen.

## Non-Goals

- Running WordPress PHP, WP-CLI, hooks, filters, cron, or mail paths.
- Dynamic runtime output from hooks, filters, installed themes, localization,
  cron, rewrite probing, mail notification, or PHP-generated privacy-policy
  content.
- Multisite roles, network options, plugin tables, or upgrade migrations.
- Parsing PHP serialization in MyLite.

## Compatibility Impact

Application-schema compatibility remains partial. The WordPress fixture now
covers the full deterministic single-site `populate_options()` option-name
inventory, representative default values, expected non-autoload flags, and full
default role payload storage, but full WordPress runtime install remains
planned.

## Single-File And Embedded-Lifecycle Impact

The seed still stores all durable state inside the primary `.mylite` file. No
new runtime companions, file-format state, or native WordPress dependencies are
introduced.

## Public API, Build, Size, And Dependency Impact

No public API, build-profile, binary-size, or dependency change is intended.
The fixture remains GPL-compatible test data derived from the pinned WordPress
source.

## Test Plan

- Extend the WordPress installer seed fixture and storage-smoke assertions.
- Run the storage-smoke test.
- Run `tools/mylite-compat-harness report application-schema`.
- Run release-gate build, format, tidy, shell syntax, and diff checks before
  commit.

## Acceptance Criteria

- The WordPress installer schema plus seed fixtures import successfully.
- The full deterministic single-site default-option inventory and serialized
  full role payload read back before and after close/reopen.
- No durable sidecars remain after close.
- Compatibility and roadmap docs describe the added default/role fixture
  coverage without claiming full WordPress runtime support.
