-- Representative dump fragment for out-of-order foreign-key data import.

SET foreign_key_checks=0;

CREATE TABLE dump_fk_parent (
    id int NOT NULL,
    parent_code int NOT NULL,
    label varchar(64) NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY dump_fk_parent_code (parent_code)
) ENGINE=InnoDB;

CREATE TABLE dump_fk_child (
    id int NOT NULL,
    parent_code int NULL,
    body varchar(64) NOT NULL,
    PRIMARY KEY (id),
    KEY dump_fk_child_parent_code (parent_code),
    CONSTRAINT dump_fk_child_parent FOREIGN KEY (parent_code)
        REFERENCES dump_fk_parent(parent_code)
        ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB;

INSERT INTO dump_fk_child VALUES
(1, 10, 'child-before-parent'),
(2, 20, 'orphan-from-dump');

INSERT INTO dump_fk_parent VALUES
(1, 10, 'parent-after-child');

SET foreign_key_checks=1;
