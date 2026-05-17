-- Derived from WordPress 6.9.4 wp-admin/includes/schema.php and
-- wp-admin/includes/upgrade.php single-site install paths.
--
-- Deterministic substitutions:
--   $wpdb table names -> default wp_ prefix
--   site URL -> https://example.test
--   installer title/user/email -> stable test values
--   current_time('mysql') -> 2026-05-15 12:00:00
--   current_time('mysql', true) -> 2026-05-15 10:00:00
--   time() -> 1778839200
--   $wp_db_version -> 60717
--   default role payload -> serialized single-site roles after populate_roles()

INSERT INTO wp_options (option_name, option_value, autoload) VALUES
('siteurl', 'https://example.test', 'on'),
('home', 'https://example.test', 'on'),
('blogname', 'MyLite Fixture', 'on'),
('blogdescription', '', 'on'),
('users_can_register', '0', 'on'),
('admin_email', 'admin@example.test', 'on'),
('start_of_week', '1', 'on'),
('use_balanceTags', '0', 'on'),
('use_smilies', '1', 'on'),
('require_name_email', '1', 'on'),
('comments_notify', '1', 'on'),
('posts_per_rss', '10', 'on'),
('rss_use_excerpt', '0', 'on'),
('mailserver_url', 'mail.example.com', 'on'),
('mailserver_login', 'login@example.com', 'on'),
('mailserver_pass', '', 'on'),
('mailserver_port', '110', 'on'),
('default_category', '1', 'on'),
('default_comment_status', 'open', 'on'),
('default_ping_status', 'open', 'on'),
('default_pingback_flag', '1', 'on'),
('posts_per_page', '10', 'on'),
('date_format', 'F j, Y', 'on'),
('time_format', 'g:i a', 'on'),
('links_updated_date_format', 'F j, Y g:i a', 'on'),
('comment_moderation', '0', 'on'),
('moderation_notify', '1', 'on'),
('permalink_structure', '', 'on'),
('rewrite_rules', '', 'on'),
('hack_file', '0', 'on'),
('blog_charset', 'UTF-8', 'on'),
('moderation_keys', '', 'off'),
('active_plugins', 'a:0:{}', 'on'),
('category_base', '', 'on'),
('ping_sites', 'https://rpc.pingomatic.com/', 'on'),
('comment_max_links', '2', 'on'),
('gmt_offset', '0', 'on'),
('default_email_category', '1', 'on'),
('recently_edited', '', 'off'),
('template', 'twentytwentysix', 'on'),
('stylesheet', 'twentytwentysix', 'on'),
('comment_registration', '0', 'on'),
('html_type', 'text/html', 'on'),
('use_trackback', '0', 'on'),
('default_role', 'subscriber', 'on'),
('db_version', '60717', 'on'),
('uploads_use_yearmonth_folders', '1', 'on'),
('upload_path', '', 'on'),
('blog_public', '1', 'on'),
('default_link_category', '2', 'on'),
('show_on_front', 'posts', 'on'),
('tag_base', '', 'on'),
('show_avatars', '1', 'on'),
('avatar_rating', 'G', 'on'),
('upload_url_path', '', 'on'),
('thumbnail_size_w', '150', 'on'),
('thumbnail_size_h', '150', 'on'),
('thumbnail_crop', '1', 'on'),
('medium_size_w', '300', 'on'),
('medium_size_h', '300', 'on'),
('avatar_default', 'mystery', 'on'),
('large_size_w', '1024', 'on'),
('large_size_h', '1024', 'on'),
('image_default_link_type', 'none', 'on'),
('image_default_size', '', 'on'),
('image_default_align', '', 'on'),
('close_comments_for_old_posts', '0', 'on'),
('close_comments_days_old', '14', 'on'),
('thread_comments', '1', 'on'),
('thread_comments_depth', '5', 'on'),
('page_comments', '0', 'on'),
('comments_per_page', '50', 'on'),
('default_comments_page', 'newest', 'on'),
('comment_order', 'asc', 'on'),
('sticky_posts', 'a:0:{}', 'on'),
('widget_categories', 'a:0:{}', 'on'),
('widget_text', 'a:0:{}', 'on'),
('widget_rss', 'a:0:{}', 'on'),
('uninstall_plugins', 'a:0:{}', 'off'),
('timezone_string', '', 'on'),
('page_for_posts', '0', 'on'),
('page_on_front', '0', 'on'),
('default_post_format', '0', 'on'),
('link_manager_enabled', '0', 'on'),
('finished_splitting_shared_terms', '1', 'on'),
('site_icon', '0', 'on'),
('medium_large_size_w', '768', 'on'),
('medium_large_size_h', '0', 'on'),
('wp_page_for_privacy_policy', '0', 'on'),
('show_comments_cookies_opt_in', '1', 'on'),
('admin_email_lifespan', '1794391200', 'on'),
('disallowed_keys', '', 'off'),
('comment_previously_approved', '1', 'on'),
('auto_plugin_theme_update_emails', 'a:0:{}', 'off'),
('auto_update_core_dev', 'enabled', 'on'),
('auto_update_core_minor', 'enabled', 'on'),
('auto_update_core_major', 'enabled', 'on'),
('wp_force_deactivated_plugins', 'a:0:{}', 'on'),
('wp_attachment_pages_enabled', '0', 'on'),
('wp_notes_notify', '1', 'on'),
('fresh_site', '1', 'off'),
('initial_db_version', '60717', 'on'),
('widget_block', 'a:1:{i:2;a:1:{s:7:"content";s:19:"<!-- wp:search /-->";}}', 'on'),
('wp_user_roles', 'a:5:{s:13:"administrator";a:2:{s:4:"name";s:13:"Administrator";s:12:"capabilities";a:61:{s:13:"switch_themes";b:1;s:11:"edit_themes";b:1;s:16:"activate_plugins";b:1;s:12:"edit_plugins";b:1;s:10:"edit_users";b:1;s:10:"edit_files";b:1;s:14:"manage_options";b:1;s:17:"moderate_comments";b:1;s:17:"manage_categories";b:1;s:12:"manage_links";b:1;s:12:"upload_files";b:1;s:6:"import";b:1;s:15:"unfiltered_html";b:1;s:10:"edit_posts";b:1;s:17:"edit_others_posts";b:1;s:20:"edit_published_posts";b:1;s:13:"publish_posts";b:1;s:10:"edit_pages";b:1;s:4:"read";b:1;s:8:"level_10";b:1;s:7:"level_9";b:1;s:7:"level_8";b:1;s:7:"level_7";b:1;s:7:"level_6";b:1;s:7:"level_5";b:1;s:7:"level_4";b:1;s:7:"level_3";b:1;s:7:"level_2";b:1;s:7:"level_1";b:1;s:7:"level_0";b:1;s:17:"edit_others_pages";b:1;s:20:"edit_published_pages";b:1;s:13:"publish_pages";b:1;s:12:"delete_pages";b:1;s:19:"delete_others_pages";b:1;s:22:"delete_published_pages";b:1;s:12:"delete_posts";b:1;s:19:"delete_others_posts";b:1;s:22:"delete_published_posts";b:1;s:20:"delete_private_posts";b:1;s:18:"edit_private_posts";b:1;s:18:"read_private_posts";b:1;s:20:"delete_private_pages";b:1;s:18:"edit_private_pages";b:1;s:18:"read_private_pages";b:1;s:12:"delete_users";b:1;s:12:"create_users";b:1;s:17:"unfiltered_upload";b:1;s:14:"edit_dashboard";b:1;s:14:"update_plugins";b:1;s:14:"delete_plugins";b:1;s:15:"install_plugins";b:1;s:13:"update_themes";b:1;s:14:"install_themes";b:1;s:11:"update_core";b:1;s:10:"list_users";b:1;s:12:"remove_users";b:1;s:13:"promote_users";b:1;s:18:"edit_theme_options";b:1;s:13:"delete_themes";b:1;s:6:"export";b:1;}}s:6:"editor";a:2:{s:4:"name";s:6:"Editor";s:12:"capabilities";a:34:{s:17:"moderate_comments";b:1;s:17:"manage_categories";b:1;s:12:"manage_links";b:1;s:12:"upload_files";b:1;s:15:"unfiltered_html";b:1;s:10:"edit_posts";b:1;s:17:"edit_others_posts";b:1;s:20:"edit_published_posts";b:1;s:13:"publish_posts";b:1;s:10:"edit_pages";b:1;s:4:"read";b:1;s:7:"level_7";b:1;s:7:"level_6";b:1;s:7:"level_5";b:1;s:7:"level_4";b:1;s:7:"level_3";b:1;s:7:"level_2";b:1;s:7:"level_1";b:1;s:7:"level_0";b:1;s:17:"edit_others_pages";b:1;s:20:"edit_published_pages";b:1;s:13:"publish_pages";b:1;s:12:"delete_pages";b:1;s:19:"delete_others_pages";b:1;s:22:"delete_published_pages";b:1;s:12:"delete_posts";b:1;s:19:"delete_others_posts";b:1;s:22:"delete_published_posts";b:1;s:20:"delete_private_posts";b:1;s:18:"edit_private_posts";b:1;s:18:"read_private_posts";b:1;s:20:"delete_private_pages";b:1;s:18:"edit_private_pages";b:1;s:18:"read_private_pages";b:1;}}s:6:"author";a:2:{s:4:"name";s:6:"Author";s:12:"capabilities";a:10:{s:12:"upload_files";b:1;s:10:"edit_posts";b:1;s:20:"edit_published_posts";b:1;s:13:"publish_posts";b:1;s:4:"read";b:1;s:7:"level_2";b:1;s:7:"level_1";b:1;s:7:"level_0";b:1;s:12:"delete_posts";b:1;s:22:"delete_published_posts";b:1;}}s:11:"contributor";a:2:{s:4:"name";s:11:"Contributor";s:12:"capabilities";a:5:{s:10:"edit_posts";b:1;s:4:"read";b:1;s:7:"level_1";b:1;s:7:"level_0";b:1;s:12:"delete_posts";b:1;}}s:10:"subscriber";a:2:{s:4:"name";s:10:"Subscriber";s:12:"capabilities";a:2:{s:4:"read";b:1;s:7:"level_0";b:1;}}}', 'on');

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
