-- Derived from BuddyPress 14.4.0 src/bp-core/admin/bp-core-admin-schema.php.
-- Full-component BuddyPress-owned table schema.
--
-- Deterministic substitutions:
--   BuddyPress table prefix -> default wp_ prefix
--   $charset_collate -> DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci
--   WordPress core wp_signups table omitted; covered by WordPress multisite fixtures

CREATE TABLE wp_bp_notifications (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	user_id bigint(20) NOT NULL,
	item_id bigint(20) NOT NULL,
	secondary_item_id bigint(20),
	component_name varchar(75) NOT NULL,
	component_action varchar(75) NOT NULL,
	date_notified datetime NOT NULL,
	is_new tinyint(1) NOT NULL DEFAULT 0,
	KEY item_id (item_id),
	KEY secondary_item_id (secondary_item_id),
	KEY user_id (user_id),
	KEY is_new (is_new),
	KEY component_name (component_name),
	KEY component_action (component_action),
	KEY useritem (user_id,is_new)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_notifications_meta (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	notification_id bigint(20) NOT NULL,
	meta_key varchar(255) DEFAULT NULL,
	meta_value longtext DEFAULT NULL,
	KEY notification_id (notification_id),
	KEY meta_key (meta_key(191))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_activity (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	user_id bigint(20) NOT NULL,
	component varchar(75) NOT NULL,
	type varchar(75) NOT NULL,
	action text NOT NULL,
	content longtext NOT NULL,
	primary_link text NOT NULL,
	item_id bigint(20) NOT NULL,
	secondary_item_id bigint(20) DEFAULT NULL,
	date_recorded datetime NOT NULL,
	hide_sitewide tinyint(1) DEFAULT 0,
	mptt_left int(11) NOT NULL DEFAULT 0,
	mptt_right int(11) NOT NULL DEFAULT 0,
	is_spam tinyint(1) NOT NULL DEFAULT 0,
	KEY date_recorded (date_recorded),
	KEY user_id (user_id),
	KEY item_id (item_id),
	KEY secondary_item_id (secondary_item_id),
	KEY component (component),
	KEY type (type),
	KEY mptt_left (mptt_left),
	KEY mptt_right (mptt_right),
	KEY hide_sitewide (hide_sitewide),
	KEY is_spam (is_spam)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_activity_meta (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	activity_id bigint(20) NOT NULL,
	meta_key varchar(255) DEFAULT NULL,
	meta_value longtext DEFAULT NULL,
	KEY activity_id (activity_id),
	KEY meta_key (meta_key(191))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_friends (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	initiator_user_id bigint(20) NOT NULL,
	friend_user_id bigint(20) NOT NULL,
	is_confirmed tinyint(1) DEFAULT 0,
	is_limited tinyint(1) DEFAULT 0,
	date_created datetime NOT NULL,
	KEY initiator_user_id (initiator_user_id),
	KEY friend_user_id (friend_user_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_groups (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	creator_id bigint(20) NOT NULL,
	name varchar(100) NOT NULL,
	slug varchar(200) NOT NULL,
	description longtext NOT NULL,
	status varchar(10) NOT NULL DEFAULT 'public',
	parent_id bigint(20) NOT NULL DEFAULT 0,
	enable_forum tinyint(1) NOT NULL DEFAULT '1',
	date_created datetime NOT NULL,
	KEY creator_id (creator_id),
	KEY status (status),
	KEY parent_id (parent_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_groups_members (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	group_id bigint(20) NOT NULL,
	user_id bigint(20) NOT NULL,
	inviter_id bigint(20) NOT NULL,
	is_admin tinyint(1) NOT NULL DEFAULT '0',
	is_mod tinyint(1) NOT NULL DEFAULT '0',
	user_title varchar(100) NOT NULL,
	date_modified datetime NOT NULL,
	comments longtext NOT NULL,
	is_confirmed tinyint(1) NOT NULL DEFAULT '0',
	is_banned tinyint(1) NOT NULL DEFAULT '0',
	invite_sent tinyint(1) NOT NULL DEFAULT '0',
	KEY group_id (group_id),
	KEY is_admin (is_admin),
	KEY is_mod (is_mod),
	KEY user_id (user_id),
	KEY inviter_id (inviter_id),
	KEY is_confirmed (is_confirmed)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_groups_groupmeta (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	group_id bigint(20) NOT NULL,
	meta_key varchar(255) DEFAULT NULL,
	meta_value longtext DEFAULT NULL,
	KEY group_id (group_id),
	KEY meta_key (meta_key(191))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_messages_messages (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	thread_id bigint(20) NOT NULL,
	sender_id bigint(20) NOT NULL,
	subject varchar(200) NOT NULL,
	message longtext NOT NULL,
	date_sent datetime NOT NULL,
	KEY sender_id (sender_id),
	KEY thread_id (thread_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_messages_recipients (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	user_id bigint(20) NOT NULL,
	thread_id bigint(20) NOT NULL,
	unread_count int(10) NOT NULL DEFAULT '0',
	sender_only tinyint(1) NOT NULL DEFAULT '0',
	is_deleted tinyint(1) NOT NULL DEFAULT '0',
	KEY user_id (user_id),
	KEY thread_id (thread_id),
	KEY is_deleted (is_deleted),
	KEY sender_only (sender_only),
	KEY unread_count (unread_count)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_messages_notices (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	subject varchar(200) NOT NULL,
	message longtext NOT NULL,
	date_sent datetime NOT NULL,
	is_active tinyint(1) NOT NULL DEFAULT '0',
	KEY is_active (is_active)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_messages_meta (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	message_id bigint(20) NOT NULL,
	meta_key varchar(255) DEFAULT NULL,
	meta_value longtext DEFAULT NULL,
	KEY message_id (message_id),
	KEY meta_key (meta_key(191))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_xprofile_groups (
	id bigint(20) unsigned NOT NULL AUTO_INCREMENT PRIMARY KEY,
	name varchar(150) NOT NULL,
	description mediumtext NOT NULL,
	group_order bigint(20) NOT NULL DEFAULT '0',
	can_delete tinyint(1) NOT NULL,
	KEY can_delete (can_delete)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_xprofile_fields (
	id bigint(20) unsigned NOT NULL AUTO_INCREMENT PRIMARY KEY,
	group_id bigint(20) unsigned NOT NULL,
	parent_id bigint(20) unsigned NOT NULL,
	type varchar(150) NOT NULL,
	name varchar(150) NOT NULL,
	description longtext NOT NULL,
	is_required tinyint(1) NOT NULL DEFAULT '0',
	is_default_option tinyint(1) NOT NULL DEFAULT '0',
	field_order bigint(20) NOT NULL DEFAULT '0',
	option_order bigint(20) NOT NULL DEFAULT '0',
	order_by varchar(15) NOT NULL DEFAULT '',
	can_delete tinyint(1) NOT NULL DEFAULT '1',
	KEY group_id (group_id),
	KEY parent_id (parent_id),
	KEY field_order (field_order),
	KEY can_delete (can_delete),
	KEY is_required (is_required)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_xprofile_data (
	id bigint(20) unsigned NOT NULL AUTO_INCREMENT PRIMARY KEY,
	field_id bigint(20) unsigned NOT NULL,
	user_id bigint(20) unsigned NOT NULL,
	value longtext NOT NULL,
	last_updated datetime NOT NULL,
	KEY field_id (field_id),
	KEY user_id (user_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_xprofile_meta (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	object_id bigint(20) NOT NULL,
	object_type varchar(150) NOT NULL,
	meta_key varchar(255) DEFAULT NULL,
	meta_value longtext DEFAULT NULL,
	KEY object_id (object_id),
	KEY meta_key (meta_key(191))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_user_blogs (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	user_id bigint(20) NOT NULL,
	blog_id bigint(20) NOT NULL,
	KEY user_id (user_id),
	KEY blog_id (blog_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_user_blogs_blogmeta (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	blog_id bigint(20) NOT NULL,
	meta_key varchar(255) DEFAULT NULL,
	meta_value longtext DEFAULT NULL,
	KEY blog_id (blog_id),
	KEY meta_key (meta_key(191))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_invitations (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	user_id bigint(20) NOT NULL,
	inviter_id bigint(20) NOT NULL,
	invitee_email varchar(100) DEFAULT NULL,
	class varchar(120) NOT NULL,
	item_id bigint(20) NOT NULL,
	secondary_item_id bigint(20) DEFAULT NULL,
	type varchar(12) NOT NULL DEFAULT 'invite',
	content longtext DEFAULT '',
	date_modified datetime NOT NULL,
	invite_sent tinyint(1) NOT NULL DEFAULT '0',
	accepted tinyint(1) NOT NULL DEFAULT '0',
	KEY user_id (user_id),
	KEY inviter_id (inviter_id),
	KEY invitee_email (invitee_email),
	KEY class (class),
	KEY item_id (item_id),
	KEY secondary_item_id (secondary_item_id),
	KEY type (type),
	KEY invite_sent (invite_sent),
	KEY accepted (accepted)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_bp_optouts (
	id bigint(20) NOT NULL AUTO_INCREMENT PRIMARY KEY,
	email_address_hash varchar(255) NOT NULL,
	user_id bigint(20) NOT NULL,
	email_type varchar(255) NOT NULL,
	date_modified datetime NOT NULL,
	KEY user_id (user_id),
	KEY email_type (email_type),
	KEY date_modified (date_modified)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
