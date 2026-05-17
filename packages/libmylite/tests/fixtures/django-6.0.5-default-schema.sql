-- Derived from django/django 6.0.5 default auth, admin, contenttypes,
-- sessions, and migration-recorder schema.
-- Deterministic MySQL/MariaDB DDL equivalent for MyLite storage-smoke tests.
--
-- Deterministic substitutions:
--   AutoField -> integer auto_increment primary key
--   BigAutoField -> bigint auto_increment primary key
--   DateTimeField -> datetime(6)
--   TextField -> longtext
--   BooleanField -> bool
--   PositiveSmallIntegerField -> smallint unsigned
--   No foreign-key constraints; MyLite rejects them until FK support exists

CREATE TABLE django_migrations (
	id bigint NOT NULL AUTO_INCREMENT,
	app varchar(255) NOT NULL,
	name varchar(255) NOT NULL,
	applied datetime(6) NOT NULL,
	PRIMARY KEY (id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE django_content_type (
	id integer NOT NULL AUTO_INCREMENT,
	app_label varchar(100) NOT NULL,
	model varchar(100) NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY django_content_type_app_label_model_unique (app_label, model)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE auth_permission (
	id integer NOT NULL AUTO_INCREMENT,
	name varchar(255) NOT NULL,
	content_type_id integer NOT NULL,
	codename varchar(100) NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY auth_permission_content_type_id_codename_unique (content_type_id, codename)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE auth_group (
	id integer NOT NULL AUTO_INCREMENT,
	name varchar(150) NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY auth_group_name_unique (name)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE auth_group_permissions (
	id integer NOT NULL AUTO_INCREMENT,
	group_id integer NOT NULL,
	permission_id integer NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY auth_group_permissions_group_id_permission_id_unique (group_id, permission_id),
	KEY auth_group_permissions_permission_id_index (permission_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE auth_user (
	id integer NOT NULL AUTO_INCREMENT,
	password varchar(128) NOT NULL,
	last_login datetime(6) DEFAULT NULL,
	is_superuser bool NOT NULL,
	username varchar(150) NOT NULL,
	first_name varchar(150) NOT NULL,
	last_name varchar(150) NOT NULL,
	email varchar(254) NOT NULL,
	is_staff bool NOT NULL,
	is_active bool NOT NULL,
	date_joined datetime(6) NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY auth_user_username_unique (username)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE auth_user_groups (
	id integer NOT NULL AUTO_INCREMENT,
	user_id integer NOT NULL,
	group_id integer NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY auth_user_groups_user_id_group_id_unique (user_id, group_id),
	KEY auth_user_groups_group_id_index (group_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE auth_user_user_permissions (
	id integer NOT NULL AUTO_INCREMENT,
	user_id integer NOT NULL,
	permission_id integer NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY auth_user_user_permissions_user_id_permission_id_unique (user_id, permission_id),
	KEY auth_user_user_permissions_permission_id_index (permission_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE django_admin_log (
	id integer NOT NULL AUTO_INCREMENT,
	action_time datetime(6) NOT NULL,
	object_id longtext DEFAULT NULL,
	object_repr varchar(200) NOT NULL,
	action_flag smallint unsigned NOT NULL,
	change_message longtext NOT NULL,
	content_type_id integer DEFAULT NULL,
	user_id integer NOT NULL,
	PRIMARY KEY (id),
	KEY django_admin_log_content_type_id_index (content_type_id),
	KEY django_admin_log_user_id_index (user_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE django_session (
	session_key varchar(40) NOT NULL,
	session_data longtext NOT NULL,
	expire_date datetime(6) NOT NULL,
	PRIMARY KEY (session_key),
	KEY django_session_expire_date_index (expire_date)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
