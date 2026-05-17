# WordPress Installer Default Role Fixture

## Problem

The WordPress single-site seed fixture proves representative installer data,
but its role option was intentionally minimal. The roadmap still calls out
installer defaults and roles as planned application-schema coverage. MyLite
needs more pressure on long serialized `wp_options` values and default-option
rows without claiming a full PHP runtime install.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

WordPress source: `WordPress/WordPress` tag `6.9.4`
(`97b7f62adb5d8864c3fac554bc7182d9fd754a41`).

- `wp-admin/includes/upgrade.php:79-80` calls `populate_options()` and
  `populate_roles()` during install.
- `wp-admin/includes/schema.php:411-566` defines the default option array,
  including `users_can_register`, `start_of_week`, `use_smilies`,
  `comments_notify`, `posts_per_page`, serialized empty option arrays,
  `comment_previously_approved`, `default_role`, and core auto-update defaults.
- `wp-admin/includes/schema.php:719-734` runs the role population chain.
- `wp-admin/includes/schema.php:750-967` creates the default administrator,
  editor, author, contributor, and subscriber roles and adds the versioned
  capability set used by a single-site install.
- MyLite already stores `LONGTEXT` option values and tests semicolons inside
  serialized SQL string literals through the WordPress seed fixture executor.

## Design

Broaden the deterministic `wordpress-6.9.4-single-site-seed.sql` fixture:

- keep the existing stable site/user/timestamp substitutions;
- add representative default option rows from `populate_options()`;
- replace the minimal `wp_user_roles` payload with the serialized default
  single-site roles and capabilities for administrator, editor, author,
  contributor, and subscriber.

Extend the storage-smoke WordPress installer test to assert:

- selected default option names and values are present;
- `default_role` remains `subscriber`;
- the role payload is a long serialized option containing all five role names
  and representative administrator capabilities;
- the existing user role assignment and installer content still read back
  before and after close/reopen.

## Supported Scope

- Representative WordPress 6.9.4 single-site default options.
- Full default role-name set and capability payload as a serialized
  `wp_user_roles` option.
- Indexed `wp_options` reads and long serialized `LONGTEXT` persistence across
  close/reopen.

## Non-Goals

- Running WordPress PHP, WP-CLI, hooks, filters, cron, or mail paths.
- Exhaustively asserting every `populate_options()` default value.
- Multisite roles, network options, plugin tables, or upgrade migrations.
- Parsing PHP serialization in MyLite.

## Compatibility Impact

Application-schema compatibility remains partial. The WordPress fixture now
covers broader installer default options and full default role payload storage,
but full WordPress runtime install remains planned.

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
- Broader default option rows and the serialized full role payload read back
  before and after close/reopen.
- No durable sidecars remain after close.
- Compatibility and roadmap docs describe the added default/role fixture
  coverage without claiming full WordPress runtime support.
