-- Derived from laravel/laravel v13.6.0 default database migrations.
-- Deterministic MySQL/MariaDB DDL equivalent for the skeleton application.
--
-- Deterministic substitutions:
--   Blueprint id() -> bigint unsigned auto_increment primary key
--   Blueprint string() -> varchar(255), unless a shorter length is specified
--   Blueprint timestamps() -> nullable created_at / updated_at timestamp columns
--   foreignId()->nullable()->index() -> nullable unsigned bigint plus index
--   No foreign-key constraints; MyLite rejects them until FK support exists

CREATE TABLE users (
	id bigint unsigned NOT NULL AUTO_INCREMENT,
	name varchar(255) NOT NULL,
	email varchar(255) NOT NULL,
	email_verified_at timestamp NULL DEFAULT NULL,
	password varchar(255) NOT NULL,
	remember_token varchar(100) DEFAULT NULL,
	created_at timestamp NULL DEFAULT NULL,
	updated_at timestamp NULL DEFAULT NULL,
	PRIMARY KEY (id),
	UNIQUE KEY users_email_unique (email)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE password_reset_tokens (
	email varchar(255) NOT NULL,
	token varchar(255) NOT NULL,
	created_at timestamp NULL DEFAULT NULL,
	PRIMARY KEY (email)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE sessions (
	id varchar(255) NOT NULL,
	user_id bigint unsigned DEFAULT NULL,
	ip_address varchar(45) DEFAULT NULL,
	user_agent text DEFAULT NULL,
	payload longtext NOT NULL,
	last_activity int NOT NULL,
	PRIMARY KEY (id),
	KEY sessions_user_id_index (user_id),
	KEY sessions_last_activity_index (last_activity)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE `cache` (
	`key` varchar(255) NOT NULL,
	value mediumtext NOT NULL,
	expiration bigint NOT NULL,
	PRIMARY KEY (`key`),
	KEY cache_expiration_index (expiration)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE cache_locks (
	`key` varchar(255) NOT NULL,
	owner varchar(255) NOT NULL,
	expiration bigint NOT NULL,
	PRIMARY KEY (`key`),
	KEY cache_locks_expiration_index (expiration)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE jobs (
	id bigint unsigned NOT NULL AUTO_INCREMENT,
	queue varchar(255) NOT NULL,
	payload longtext NOT NULL,
	attempts smallint unsigned NOT NULL,
	reserved_at int unsigned DEFAULT NULL,
	available_at int unsigned NOT NULL,
	created_at int unsigned NOT NULL,
	PRIMARY KEY (id),
	KEY jobs_queue_index (queue)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE job_batches (
	id varchar(255) NOT NULL,
	name varchar(255) NOT NULL,
	total_jobs int NOT NULL,
	pending_jobs int NOT NULL,
	failed_jobs int NOT NULL,
	failed_job_ids longtext NOT NULL,
	options mediumtext DEFAULT NULL,
	cancelled_at int DEFAULT NULL,
	created_at int NOT NULL,
	finished_at int DEFAULT NULL,
	PRIMARY KEY (id)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE failed_jobs (
	id bigint unsigned NOT NULL AUTO_INCREMENT,
	uuid varchar(255) NOT NULL,
	connection varchar(255) NOT NULL,
	queue varchar(255) NOT NULL,
	payload longtext NOT NULL,
	exception longtext NOT NULL,
	failed_at timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
	PRIMARY KEY (id),
	UNIQUE KEY failed_jobs_uuid_unique (uuid),
	KEY failed_jobs_connection_queue_failed_at_index (connection, queue, failed_at)
) DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
