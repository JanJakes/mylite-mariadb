-- Representative Django rows for the Django 6.0.5 default schema fixture.
-- Values are deterministic and cover selected migration-recorder, auth,
-- contenttype, admin-log, session, unique, composite, and secondary-index paths.
-- They are not a full Django project install.

INSERT INTO django_migrations
	(app, name, applied)
VALUES
	('contenttypes', '0001_initial', '2026-05-15 12:00:00.000000'),
	('contenttypes', '0002_remove_content_type_name', '2026-05-15 12:00:01.000000'),
	('auth', '0001_initial', '2026-05-15 12:00:02.000000'),
	('auth', '0002_alter_permission_name_max_length', '2026-05-15 12:00:03.000000'),
	('auth', '0003_alter_user_email_max_length', '2026-05-15 12:00:04.000000'),
	('auth', '0004_alter_user_username_opts', '2026-05-15 12:00:05.000000'),
	('auth', '0005_alter_user_last_login_null', '2026-05-15 12:00:06.000000'),
	('auth', '0006_require_contenttypes_0002', '2026-05-15 12:00:07.000000'),
	('auth', '0007_alter_validators_add_error_messages', '2026-05-15 12:00:08.000000'),
	('auth', '0008_alter_user_username_max_length', '2026-05-15 12:00:09.000000'),
	('auth', '0009_alter_user_last_name_max_length', '2026-05-15 12:00:10.000000'),
	('auth', '0010_alter_group_name_max_length', '2026-05-15 12:00:11.000000'),
	('auth', '0011_update_proxy_permissions', '2026-05-15 12:00:12.000000'),
	('auth', '0012_alter_user_first_name_max_length', '2026-05-15 12:00:13.000000'),
	('admin', '0001_initial', '2026-05-15 12:00:14.000000'),
	('admin', '0002_logentry_remove_auto_add', '2026-05-15 12:00:15.000000'),
	('admin', '0003_logentry_add_action_flag_choices', '2026-05-15 12:00:16.000000'),
	('sessions', '0001_initial', '2026-05-15 12:00:17.000000');

INSERT INTO django_content_type
	(id, app_label, model)
VALUES
	(1, 'contenttypes', 'contenttype'),
	(2, 'auth', 'permission'),
	(3, 'auth', 'group'),
	(4, 'auth', 'user'),
	(5, 'admin', 'logentry'),
	(6, 'sessions', 'session');

INSERT INTO auth_permission
	(id, name, content_type_id, codename)
VALUES
	(1, 'Can add content type', 1, 'add_contenttype'),
	(2, 'Can change content type', 1, 'change_contenttype'),
	(3, 'Can delete content type', 1, 'delete_contenttype'),
	(4, 'Can view content type', 1, 'view_contenttype'),
	(5, 'Can add permission', 2, 'add_permission'),
	(6, 'Can change permission', 2, 'change_permission'),
	(7, 'Can delete permission', 2, 'delete_permission'),
	(8, 'Can view permission', 2, 'view_permission'),
	(9, 'Can add group', 3, 'add_group'),
	(10, 'Can change group', 3, 'change_group'),
	(11, 'Can delete group', 3, 'delete_group'),
	(12, 'Can view group', 3, 'view_group'),
	(13, 'Can add user', 4, 'add_user'),
	(14, 'Can change user', 4, 'change_user'),
	(15, 'Can delete user', 4, 'delete_user'),
	(16, 'Can view user', 4, 'view_user'),
	(17, 'Can add log entry', 5, 'add_logentry'),
	(18, 'Can change log entry', 5, 'change_logentry'),
	(19, 'Can delete log entry', 5, 'delete_logentry'),
	(20, 'Can view log entry', 5, 'view_logentry'),
	(21, 'Can add session', 6, 'add_session'),
	(22, 'Can change session', 6, 'change_session'),
	(23, 'Can delete session', 6, 'delete_session'),
	(24, 'Can view session', 6, 'view_session');

INSERT INTO auth_group
	(id, name)
VALUES
	(1, 'Editors');

INSERT INTO auth_group_permissions
	(group_id, permission_id)
VALUES
	(1, 14);

INSERT INTO auth_user
	(
		id,
		password,
		last_login,
		is_superuser,
		username,
		first_name,
		last_name,
		email,
		is_staff,
		is_active,
		date_joined
	)
VALUES
	(
		1,
		'pbkdf2_sha256$1000000$fixture$hash',
		'2026-05-15 12:10:00.000000',
		1,
		'admin',
		'Jan',
		'Fixture',
		'admin@example.test',
		1,
		1,
		'2026-05-15 12:00:00.000000'
	);

INSERT INTO auth_user_groups
	(user_id, group_id)
VALUES
	(1, 1);

INSERT INTO auth_user_user_permissions
	(user_id, permission_id)
VALUES
	(1, 16);

INSERT INTO django_session
	(session_key, session_data, expire_date)
VALUES
	(
		'django-session-fixture',
		'.eJyrVkrLz1eyUkpKLFKqBQAeXgRK:fixture-signature',
		'2026-05-22 12:00:00.000000'
	);

INSERT INTO django_admin_log
	(
		action_time,
		object_id,
		object_repr,
		action_flag,
		change_message,
		content_type_id,
		user_id
	)
VALUES
	(
		'2026-05-15 12:15:00.000000',
		'1',
		'admin',
		1,
		'[{"added": {}}]',
		4,
		1
	);
