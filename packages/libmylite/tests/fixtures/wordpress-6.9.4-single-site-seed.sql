-- Derived from WordPress 6.9.4 wp-admin/includes/schema.php and
-- wp-admin/includes/upgrade.php single-site install paths.
--
-- Deterministic substitutions:
--   $wpdb table names -> default wp_ prefix
--   site URL -> https://example.test
--   installer title/user/email -> stable test values
--   current_time('mysql') -> 2026-05-15 12:00:00
--   current_time('mysql', true) -> 2026-05-15 10:00:00
--   $wp_db_version -> 60717

INSERT INTO wp_options (option_name, option_value, autoload) VALUES
('siteurl', 'https://example.test', 'on'),
('home', 'https://example.test', 'on'),
('blogname', 'MyLite Fixture', 'on'),
('blogdescription', '', 'on'),
('admin_email', 'admin@example.test', 'on'),
('blog_public', '1', 'on'),
('fresh_site', '1', 'off'),
('default_category', '1', 'on'),
('default_comment_status', 'open', 'on'),
('default_ping_status', 'open', 'on'),
('default_pingback_flag', '1', 'on'),
('template', 'twentytwentysix', 'on'),
('stylesheet', 'twentytwentysix', 'on'),
('default_role', 'subscriber', 'on'),
('db_version', '60717', 'on'),
('initial_db_version', '60717', 'on'),
('show_on_front', 'posts', 'on'),
('wp_page_for_privacy_policy', '0', 'on'),
('wp_attachment_pages_enabled', '0', 'on'),
('wp_notes_notify', '1', 'on'),
('widget_block', 'a:1:{i:2;a:1:{s:7:"content";s:19:"<!-- wp:search /-->";}}', 'on'),
('wp_user_roles', 'a:1:{s:13:"administrator";a:2:{s:4:"name";s:13:"Administrator";s:12:"capabilities";a:1:{s:4:"read";b:1;}}}', 'on');

INSERT INTO wp_users (
	ID, user_login, user_pass, user_nicename, user_email, user_url,
	user_registered, user_activation_key, user_status, display_name
) VALUES (
	1, 'admin', '$P$Bfixturehash', 'admin', 'admin@example.test',
	'https://example.test', '2026-05-15 10:00:00', '', 0, 'admin'
);

INSERT INTO wp_usermeta (user_id, meta_key, meta_value) VALUES
(1, 'default_password_nag', '1'),
(1, 'wp_capabilities', 'a:1:{s:13:"administrator";b:1;}'),
(1, 'wp_user_level', '10'),
(1, 'show_welcome_panel', '1');

INSERT INTO wp_terms (term_id, name, slug, term_group) VALUES
(1, 'Uncategorized', 'uncategorized', 0);

INSERT INTO wp_term_taxonomy (term_id, taxonomy, description, parent, count) VALUES
(1, 'category', '', 0, 1);

INSERT INTO wp_posts (
	post_author, post_date, post_date_gmt, post_content, post_excerpt,
	post_title, post_name, post_modified, post_modified_gmt, guid,
	comment_count, to_ping, pinged, post_content_filtered
) VALUES (
	1, '2026-05-15 12:00:00', '2026-05-15 10:00:00',
	'<!-- wp:paragraph -->\n<p>Welcome to WordPress. This is your first post. Edit or delete it, then start writing!</p>\n<!-- /wp:paragraph -->',
	'', 'Hello world!', 'hello-world', '2026-05-15 12:00:00',
	'2026-05-15 10:00:00', 'https://example.test/?p=1', 1, '', '', ''
);

INSERT INTO wp_term_relationships (term_taxonomy_id, object_id) VALUES
(1, 1);

INSERT INTO wp_comments (
	comment_post_ID, comment_author, comment_author_email, comment_author_url,
	comment_date, comment_date_gmt, comment_content, comment_type
) VALUES (
	1, 'A WordPress Commenter', 'wapuu@wordpress.example', 'https://wordpress.org/',
	'2026-05-15 12:00:00', '2026-05-15 10:00:00',
	'Hi, this is a comment.\nTo get started with moderating, editing, and deleting comments, please visit the Comments screen in the dashboard.\nCommenter avatars come from <a href="https://gravatar.com/">Gravatar</a>.',
	'comment'
);

INSERT INTO wp_posts (
	post_author, post_date, post_date_gmt, post_content, post_excerpt,
	comment_status, post_title, post_name, post_modified, post_modified_gmt,
	guid, post_type, to_ping, pinged, post_content_filtered
) VALUES (
	1, '2026-05-15 12:00:00', '2026-05-15 10:00:00',
	'<!-- wp:paragraph -->\n<p>This is an example page.</p>\n<!-- /wp:paragraph -->',
	'', 'closed', 'Sample Page', 'sample-page', '2026-05-15 12:00:00',
	'2026-05-15 10:00:00', 'https://example.test/?page_id=2', 'page', '', '', ''
);

INSERT INTO wp_postmeta (post_id, meta_key, meta_value) VALUES
(2, '_wp_page_template', 'default');
