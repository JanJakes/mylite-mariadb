-- Derived from rails/rails v8.1.3 Active Record metadata, Active Storage, and
-- Action Text install migrations.
-- Deterministic MySQL/MariaDB DDL equivalent for MyLite storage-smoke tests.
--
-- Deterministic substitutions:
--   Active Record primary_key -> bigint auto_increment primary key
--   Active Record string -> varchar(255)
--   Active Record text -> text, and text size: :long -> longtext
--   Active Record datetime with precision -> datetime(6)
--   No foreign-key constraints; MyLite rejects them until FK support exists

CREATE TABLE schema_migrations (
	version varchar(255) NOT NULL,
	PRIMARY KEY (version)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE ar_internal_metadata (
	`key` varchar(255) NOT NULL,
	value varchar(255) DEFAULT NULL,
	created_at datetime(6) NOT NULL,
	updated_at datetime(6) NOT NULL,
	PRIMARY KEY (`key`)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE active_storage_blobs (
	id bigint NOT NULL AUTO_INCREMENT,
	`key` varchar(255) NOT NULL,
	filename varchar(255) NOT NULL,
	content_type varchar(255) DEFAULT NULL,
	metadata text DEFAULT NULL,
	service_name varchar(255) NOT NULL,
	byte_size bigint NOT NULL,
	checksum varchar(255) DEFAULT NULL,
	created_at datetime(6) NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY index_active_storage_blobs_on_key (`key`)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE active_storage_attachments (
	id bigint NOT NULL AUTO_INCREMENT,
	name varchar(255) NOT NULL,
	record_type varchar(255) NOT NULL,
	record_id bigint NOT NULL,
	blob_id bigint NOT NULL,
	created_at datetime(6) NOT NULL,
	PRIMARY KEY (id),
	KEY index_active_storage_attachments_on_blob_id (blob_id),
	UNIQUE KEY index_active_storage_attachments_uniqueness (
		record_type,
		record_id,
		name,
		blob_id
	)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE active_storage_variant_records (
	id bigint NOT NULL AUTO_INCREMENT,
	blob_id bigint NOT NULL,
	variation_digest varchar(255) NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY index_active_storage_variant_records_uniqueness (blob_id, variation_digest)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE action_text_rich_texts (
	id bigint NOT NULL AUTO_INCREMENT,
	name varchar(255) NOT NULL,
	body longtext DEFAULT NULL,
	record_type varchar(255) NOT NULL,
	record_id bigint NOT NULL,
	created_at datetime(6) NOT NULL,
	updated_at datetime(6) NOT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY index_action_text_rich_texts_uniqueness (record_type, record_id, name)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
