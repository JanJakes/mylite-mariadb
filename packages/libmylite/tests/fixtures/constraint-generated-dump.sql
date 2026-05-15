-- Representative application dump fragment for CHECK and generated columns.

CREATE TABLE dump_posts (
	id int NOT NULL,
	title varchar(64) NOT NULL,
	rating int NOT NULL,
	title_len int AS (CHAR_LENGTH(title)) VIRTUAL,
	title_key varchar(80) AS (LOWER(title)) STORED,
	PRIMARY KEY (id),
	UNIQUE KEY title_key_unique (title_key),
	KEY title_len_key (title_len),
	CONSTRAINT rating_nonnegative CHECK (rating >= 0),
	CONSTRAINT rating_cap CHECK (rating <= 10)
) ENGINE=InnoDB;

INSERT INTO dump_posts (id, title, rating) VALUES
(1, 'Alpha', 4),
(2, 'Beta', 9);
