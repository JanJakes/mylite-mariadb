-- Derived from WordPress 6.9.4 wp-admin/includes/schema.php
-- wp_get_db_schema('global') while multisite is active.
--
-- Deterministic substitutions:
--   $wpdb table names -> default wp_ prefix
--   $max_index_length -> 191
--   $charset_collate -> DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci

CREATE TABLE wp_users (
	ID bigint(20) unsigned NOT NULL auto_increment,
	user_login varchar(60) NOT NULL default '',
	user_pass varchar(255) NOT NULL default '',
	user_nicename varchar(50) NOT NULL default '',
	user_email varchar(100) NOT NULL default '',
	user_url varchar(100) NOT NULL default '',
	user_registered datetime NOT NULL default '0000-00-00 00:00:00',
	user_activation_key varchar(255) NOT NULL default '',
	user_status int(11) NOT NULL default '0',
	display_name varchar(250) NOT NULL default '',
	spam tinyint(2) NOT NULL default '0',
	deleted tinyint(2) NOT NULL default '0',
	PRIMARY KEY  (ID),
	KEY user_login_key (user_login),
	KEY user_nicename (user_nicename),
	KEY user_email (user_email)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_usermeta (
	umeta_id bigint(20) unsigned NOT NULL auto_increment,
	user_id bigint(20) unsigned NOT NULL default '0',
	meta_key varchar(255) default NULL,
	meta_value longtext,
	PRIMARY KEY  (umeta_id),
	KEY user_id (user_id),
	KEY meta_key (meta_key(191))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_blogs (
	blog_id bigint(20) unsigned NOT NULL auto_increment,
	site_id bigint(20) unsigned NOT NULL default '0',
	domain varchar(200) NOT NULL default '',
	path varchar(100) NOT NULL default '',
	registered datetime NOT NULL default '0000-00-00 00:00:00',
	last_updated datetime NOT NULL default '0000-00-00 00:00:00',
	public tinyint(2) NOT NULL default '1',
	archived tinyint(2) NOT NULL default '0',
	mature tinyint(2) NOT NULL default '0',
	spam tinyint(2) NOT NULL default '0',
	deleted tinyint(2) NOT NULL default '0',
	lang_id int(11) NOT NULL default '0',
	PRIMARY KEY  (blog_id),
	KEY domain (domain(50),path(5)),
	KEY lang_id (lang_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_blogmeta (
	meta_id bigint(20) unsigned NOT NULL auto_increment,
	blog_id bigint(20) unsigned NOT NULL default '0',
	meta_key varchar(255) default NULL,
	meta_value longtext,
	PRIMARY KEY  (meta_id),
	KEY meta_key (meta_key(191)),
	KEY blog_id (blog_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_registration_log (
	ID bigint(20) unsigned NOT NULL auto_increment,
	email varchar(255) NOT NULL default '',
	IP varchar(30) NOT NULL default '',
	blog_id bigint(20) unsigned NOT NULL default '0',
	date_registered datetime NOT NULL default '0000-00-00 00:00:00',
	PRIMARY KEY  (ID),
	KEY IP (IP)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_site (
	id bigint(20) unsigned NOT NULL auto_increment,
	domain varchar(200) NOT NULL default '',
	path varchar(100) NOT NULL default '',
	PRIMARY KEY  (id),
	KEY domain (domain(140),path(51))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_sitemeta (
	meta_id bigint(20) unsigned NOT NULL auto_increment,
	site_id bigint(20) unsigned NOT NULL default '0',
	meta_key varchar(255) default NULL,
	meta_value longtext,
	PRIMARY KEY  (meta_id),
	KEY meta_key (meta_key(191)),
	KEY site_id (site_id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE wp_signups (
	signup_id bigint(20) unsigned NOT NULL auto_increment,
	domain varchar(200) NOT NULL default '',
	path varchar(100) NOT NULL default '',
	title longtext NOT NULL,
	user_login varchar(60) NOT NULL default '',
	user_email varchar(100) NOT NULL default '',
	registered datetime NOT NULL default '0000-00-00 00:00:00',
	activated datetime NOT NULL default '0000-00-00 00:00:00',
	active tinyint(1) NOT NULL default '0',
	activation_key varchar(50) NOT NULL default '',
	meta longtext,
	PRIMARY KEY  (signup_id),
	KEY activation_key (activation_key),
	KEY user_email (user_email),
	KEY user_login_email (user_login,user_email),
	KEY domain_path (domain(140),path(51))
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
