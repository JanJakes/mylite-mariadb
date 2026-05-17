-- Representative BuddyPress rows for the BuddyPress 14.4.0 component schema fixture.
-- Values are deterministic and cover selected primary, secondary, composite,
-- and prefix-index paths. They are not a full BuddyPress install.

INSERT INTO wp_bp_notifications
	(
		user_id,
		item_id,
		secondary_item_id,
		component_name,
		component_action,
		date_notified,
		is_new
	)
VALUES
	(10, 200, 201, 'groups', 'membership_request', '2026-05-15 12:00:00', 1);

INSERT INTO wp_bp_notifications_meta
	(notification_id, meta_key, meta_value)
VALUES
	(1, 'source', 'fixture');

INSERT INTO wp_bp_activity
	(
		user_id,
		component,
		type,
		action,
		content,
		primary_link,
		item_id,
		secondary_item_id,
		date_recorded,
		hide_sitewide,
		mptt_left,
		mptt_right,
		is_spam
	)
VALUES
	(
		10,
		'activity',
		'activity_update',
		'Jan posted an update',
		'BuddyPress fixture activity',
		'https://example.test/members/jan/activity/1/',
		0,
		NULL,
		'2026-05-15 12:05:00',
		0,
		1,
		2,
		0
	);

INSERT INTO wp_bp_activity_meta
	(activity_id, meta_key, meta_value)
VALUES
	(1, 'favorite_count', '2');

INSERT INTO wp_bp_friends
	(initiator_user_id, friend_user_id, is_confirmed, is_limited, date_created)
VALUES
	(10, 11, 1, 0, '2026-05-15 12:10:00');

INSERT INTO wp_bp_groups
	(creator_id, name, slug, description, status, parent_id, enable_forum, date_created)
VALUES
	(10, 'Fixture Group', 'fixture-group', 'BuddyPress fixture group', 'public', 0, 1, '2026-05-15 12:15:00');

INSERT INTO wp_bp_groups_members
	(
		group_id,
		user_id,
		inviter_id,
		is_admin,
		is_mod,
		user_title,
		date_modified,
		comments,
		is_confirmed,
		is_banned,
		invite_sent
	)
VALUES
	(1, 10, 0, 1, 0, 'Admin', '2026-05-15 12:16:00', '', 1, 0, 0);

INSERT INTO wp_bp_groups_groupmeta
	(group_id, meta_key, meta_value)
VALUES
	(1, 'last_activity', '2026-05-15 12:17:00');

INSERT INTO wp_bp_messages_messages
	(thread_id, sender_id, subject, message, date_sent)
VALUES
	(501, 10, 'Fixture message', 'BuddyPress fixture private message', '2026-05-15 12:20:00');

INSERT INTO wp_bp_messages_recipients
	(user_id, thread_id, unread_count, sender_only, is_deleted)
VALUES
	(11, 501, 1, 0, 0);

INSERT INTO wp_bp_messages_notices
	(subject, message, date_sent, is_active)
VALUES
	('Fixture notice', 'BuddyPress fixture notice', '2026-05-15 12:25:00', 1);

INSERT INTO wp_bp_messages_meta
	(message_id, meta_key, meta_value)
VALUES
	(1, 'priority', 'high');

INSERT INTO wp_bp_xprofile_groups
	(name, description, group_order, can_delete)
VALUES
	('General', '', 0, 0);

INSERT INTO wp_bp_xprofile_fields
	(
		group_id,
		parent_id,
		type,
		name,
		description,
		is_required,
		is_default_option,
		field_order,
		option_order,
		order_by,
		can_delete
	)
VALUES
	(1, 0, 'textbox', 'Display Name', '', 1, 0, 0, 0, '', 0);

INSERT INTO wp_bp_xprofile_data
	(field_id, user_id, value, last_updated)
VALUES
	(1, 10, 'Jan Fixture', '2026-05-15 12:30:00');

INSERT INTO wp_bp_xprofile_meta
	(object_id, object_type, meta_key, meta_value)
VALUES
	(1, 'field', 'allow_custom_visibility', 'disabled'),
	(1, 'field', 'signup_position', '1');

INSERT INTO wp_bp_user_blogs
	(user_id, blog_id)
VALUES
	(10, 2);

INSERT INTO wp_bp_user_blogs_blogmeta
	(blog_id, meta_key, meta_value)
VALUES
	(2, 'last_activity', '2026-05-15 12:35:00');

INSERT INTO wp_bp_invitations
	(
		user_id,
		inviter_id,
		invitee_email,
		class,
		item_id,
		secondary_item_id,
		type,
		content,
		date_modified,
		invite_sent,
		accepted
	)
VALUES
	(
		0,
		10,
		'invitee@example.test',
		'BP_Members_Invitation_Manager',
		1,
		NULL,
		'invite',
		'Join the fixture site',
		'2026-05-15 12:40:00',
		1,
		0
	);

INSERT INTO wp_bp_optouts
	(email_address_hash, user_id, email_type, date_modified)
VALUES
	('hash-fixture', 10, 'members-invitation', '2026-05-15 12:45:00');
