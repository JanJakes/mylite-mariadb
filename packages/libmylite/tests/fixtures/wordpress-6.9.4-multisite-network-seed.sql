-- Derived from WordPress 6.9.4 wp-admin/includes/schema.php network
-- population paths and representative multisite blog starter content.
--
-- Deterministic substitutions:
--   network domain -> example.test
--   network path -> /
--   main blog path -> /
--   representative blog id/path -> 2 /second/
--   network administrator -> network-admin
--   timestamps -> fixed fixture values

INSERT INTO wp_site (id, domain, path) VALUES
(1, 'example.test', '/');

INSERT INTO wp_sitemeta (site_id, meta_key, meta_value) VALUES
(1, 'site_name', 'MyLite Network'),
(1, 'admin_email', 'admin@example.test'),
(1, 'admin_user_id', '1'),
(1, 'registration', 'none'),
(1, 'site_admins', 'a:1:{i:0;s:13:"network-admin";}'),
(1, 'active_sitewide_plugins', 'a:0:{}'),
(1, 'subdomain_install', '0'),
(1, 'main_site', '1');

INSERT INTO wp_blogs (
	blog_id, site_id, domain, path, registered, last_updated
) VALUES
(1, 1, 'example.test', '/', '2026-05-15 12:00:00', '2026-05-15 12:00:00'),
(2, 1, 'example.test', '/second/', '2026-05-15 12:01:00', '2026-05-15 12:01:00');

INSERT INTO wp_blogmeta (blog_id, meta_key, meta_value) VALUES
(2, 'public', '1'),
(2, 'site_name', 'Second Site');

INSERT INTO wp_users (
	ID, user_login, user_pass, user_nicename, user_email, user_registered, display_name
) VALUES (
	1, 'network-admin', '$P$Bfixturehash', 'network-admin', 'admin@example.test',
	'2026-05-15 10:00:00', 'network-admin'
);

INSERT INTO wp_usermeta (user_id, meta_key, meta_value) VALUES
(1, 'source_domain', 'example.test'),
(1, 'primary_blog', '1');

INSERT INTO wp_2_options (option_name, option_value, autoload) VALUES
('siteurl', 'https://example.test/second', 'yes'),
('blogname', 'Second Site', 'yes');

INSERT INTO wp_2_terms (term_id, name, slug, term_group) VALUES
(1, 'Network News', 'network-news', 0);

INSERT INTO wp_2_termmeta (term_id, meta_key, meta_value) VALUES
(1, 'display', 'featured');

INSERT INTO wp_2_term_taxonomy (
	term_taxonomy_id, term_id, taxonomy, description, parent, count
) VALUES (
	1, 1, 'category', 'Second site category', 0, 1
);

INSERT INTO wp_2_posts (
	ID, post_author, post_date, post_date_gmt, post_content, post_title, post_excerpt,
	post_status, comment_status, ping_status, post_password, post_name, to_ping, pinged,
	post_modified, post_modified_gmt, post_content_filtered, post_parent, guid,
	menu_order, post_type, post_mime_type, comment_count
) VALUES (
	1, 1, '2026-05-15 12:30:00', '2026-05-15 10:30:00', 'Second site body',
	'Second Site Post', '', 'publish', 'open', 'open', '', 'second-site-post',
	'', '', '2026-05-15 12:31:00', '2026-05-15 10:31:00', '', 0,
	'https://example.test/second/?p=1', 0, 'post', '', 0
);

INSERT INTO wp_2_postmeta (post_id, meta_key, meta_value) VALUES
(1, '_thumbnail_id', '84');

INSERT INTO wp_2_term_relationships (object_id, term_taxonomy_id, term_order) VALUES
(1, 1, 0);

INSERT INTO wp_2_comments (
	comment_ID, comment_post_ID, comment_author, comment_author_email, comment_author_url,
	comment_author_IP, comment_date, comment_date_gmt, comment_content, comment_karma,
	comment_approved, comment_agent, comment_type, comment_parent, user_id
) VALUES (
	1, 1, 'Jan', 'jan@example.test', '', '127.0.0.1', '2026-05-15 12:40:00',
	'2026-05-15 10:40:00', 'Second site comment', 0, '1', 'mylite-test',
	'comment', 0, 1
);

INSERT INTO wp_2_commentmeta (comment_id, meta_key, meta_value) VALUES
(1, '_rating', '5');

INSERT INTO wp_2_links (
	link_id, link_url, link_name, link_image, link_target, link_description, link_visible,
	link_owner, link_rating, link_updated, link_rel, link_notes, link_rss
) VALUES (
	1, 'https://mylite.example/second', 'Second Site Link', '', '', 'Network link',
	'Y', 1, 0, '2026-05-15 12:50:00', '', 'link notes', ''
);
