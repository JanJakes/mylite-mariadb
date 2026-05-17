-- Representative Laravel rows for the Laravel v13.6.0 default schema fixture.
-- Values are deterministic and cover selected primary, unique, secondary, and
-- composite-index paths. They are not a full Laravel application install.

INSERT INTO users
	(name, email, email_verified_at, password, remember_token, created_at, updated_at)
VALUES
	(
		'Jan Fixture',
		'jan@example.test',
		'2026-05-15 12:00:00',
		'$2y$12$fixturehash',
		'remember-fixture',
		'2026-05-15 12:00:00',
		'2026-05-15 12:00:00'
	);

INSERT INTO password_reset_tokens
	(email, token, created_at)
VALUES
	('jan@example.test', 'reset-token-fixture', '2026-05-15 12:05:00');

INSERT INTO sessions
	(id, user_id, ip_address, user_agent, payload, last_activity)
VALUES
	(
		'session-fixture',
		1,
		'127.0.0.1',
		'MyLite Laravel fixture',
		'YTozOntzOjY6Il90b2tlbiI7czoxNToiZml4dHVyZS10b2tlbiI7fQ==',
		1778846400
	);

INSERT INTO `cache`
	(`key`, value, expiration)
VALUES
	('laravel_cache_fixture', 'fixture-cache-value', 1778847000);

INSERT INTO cache_locks
	(`key`, owner, expiration)
VALUES
	('laravel_lock_fixture', 'fixture-owner', 1778847060);

INSERT INTO jobs
	(queue, payload, attempts, reserved_at, available_at, created_at)
VALUES
	('default', '{"job":"FixtureJob","data":{"id":1}}', 1, NULL, 1778847100, 1778846400);

INSERT INTO job_batches
	(
		id,
		name,
		total_jobs,
		pending_jobs,
		failed_jobs,
		failed_job_ids,
		options,
		cancelled_at,
		created_at,
		finished_at
	)
VALUES
	(
		'batch-fixture',
		'Fixture batch',
		3,
		1,
		0,
		'[]',
		'{"allowFailures":false}',
		NULL,
		1778846400,
		NULL
	);

INSERT INTO failed_jobs
	(uuid, connection, queue, payload, exception, failed_at)
VALUES
	(
		'failed-job-fixture',
		'database',
		'default',
		'{"job":"FailedFixtureJob"}',
		'RuntimeException: fixture failure',
		'2026-05-15 12:20:00'
	);
