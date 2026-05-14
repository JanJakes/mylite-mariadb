#include <mylite/mylite.h>

#include <assert.h>
#include <stddef.h>
#include <string.h>

int main(void) {
    mylite_db *db = (mylite_db *)1;

    assert(mylite_close(NULL) == MYLITE_OK);
    assert(mylite_open(NULL, &db) == MYLITE_MISUSE);
    assert(db == NULL);
    assert(mylite_open("", &db) == MYLITE_MISUSE);
    assert(db == NULL);
    assert(mylite_open("unused.mylite", NULL) == MYLITE_MISUSE);

    assert(mylite_open_v2("unused.mylite", &db, 0U, NULL) == MYLITE_MISUSE);
    assert(db == NULL);
    assert(
        mylite_open_v2("unused.mylite", &db, MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE, NULL) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);
    assert(
        mylite_open_v2("unused.mylite", &db, MYLITE_OPEN_READONLY | MYLITE_OPEN_CREATE, NULL) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);
    assert(
        mylite_open_v2("unused.mylite", &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_URI, NULL) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);

    mylite_open_config config = {
        .size = sizeof(config),
        .profile = 99,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = NULL,
    };
    assert(
        mylite_open_v2("unused.mylite", &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_MISUSE
    );
    assert(db == NULL);

    assert(mylite_errcode(NULL) == MYLITE_MISUSE);
    assert(mylite_extended_errcode(NULL) == MYLITE_MISUSE);
    assert(mylite_mariadb_errno(NULL) == 0U);
    assert(strcmp(mylite_sqlstate(NULL), "HY000") == 0);
    assert(strcmp(mylite_errmsg(NULL), "bad database handle") == 0);
    assert(mylite_exec(NULL, "SELECT 1", NULL, NULL, NULL) == MYLITE_MISUSE);
    db = (mylite_db *)1;
    assert(mylite_exec(db, NULL, NULL, NULL, NULL) == MYLITE_MISUSE);
    assert(mylite_changes(NULL) == 0);
    assert(mylite_last_insert_id(NULL) == 0U);
    mylite_free(NULL);

    return 0;
}
