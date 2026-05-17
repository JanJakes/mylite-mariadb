-- Representative Rails rows for the Rails v8.1.3 Active Record schema fixture.
-- Values are deterministic and cover selected primary, unique, secondary, and
-- composite-index paths. They are not a full Rails application install.

INSERT INTO schema_migrations
	(version)
VALUES
	('20170806125915'),
	('20180528164100');

INSERT INTO ar_internal_metadata
	(`key`, value, created_at, updated_at)
VALUES
	('environment', 'test', '2026-05-15 12:00:00.000000', '2026-05-15 12:00:00.000000'),
	('schema_sha1', 'rails-fixture-sha1', '2026-05-15 12:00:01.000000', '2026-05-15 12:00:01.000000');

INSERT INTO active_storage_blobs
	(`key`, filename, content_type, metadata, service_name, byte_size, checksum, created_at)
VALUES
	(
		'rails-fixture-key',
		'avatar.png',
		'image/png',
		'{"identified":true,"width":64,"height":64}',
		'local',
		4096,
		'railsfixturechecksum',
		'2026-05-15 12:05:00.000000'
	);

INSERT INTO active_storage_attachments
	(name, record_type, record_id, blob_id, created_at)
VALUES
	('avatar', 'User', 1, 1, '2026-05-15 12:06:00.000000');

INSERT INTO active_storage_variant_records
	(blob_id, variation_digest)
VALUES
	(1, 'variant-digest-fixture');

INSERT INTO action_text_rich_texts
	(name, body, record_type, record_id, created_at, updated_at)
VALUES
	(
		'body',
		'<div class="trix-content">Rails fixture body</div>',
		'Post',
		1,
		'2026-05-15 12:07:00.000000',
		'2026-05-15 12:07:00.000000'
	);
