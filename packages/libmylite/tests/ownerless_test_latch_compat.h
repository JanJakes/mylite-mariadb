#ifndef MYLITE_OWNERLESS_TEST_LATCH_COMPAT_H
#define MYLITE_OWNERLESS_TEST_LATCH_COMPAT_H

#include <stdint.h>

#define MYLITE_TEST_OWNER_GENERATION(owner_id) ((uint64_t)(owner_id) + 1000ULL)
#define MYLITE_TEST_LATCH_OWNER_ID 9000U
#define MYLITE_TEST_LATCH_OWNER_GENERATION 19000ULL

#define mylite_ownerless_lock_table_acquire_exclusive(mapping, size, key, owner, timeout) \
    mylite_ownerless_lock_table_acquire_exclusive( \
        mapping, \
        size, \
        key, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        timeout \
    )
#define mylite_ownerless_lock_table_acquire_shared(mapping, size, key, owner, timeout) \
    mylite_ownerless_lock_table_acquire_shared( \
        mapping, \
        size, \
        key, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        timeout \
    )
#define mylite_ownerless_lock_table_release_exclusive(mapping, size, key, owner) \
    mylite_ownerless_lock_table_release_exclusive( \
        mapping, \
        size, \
        key, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner) \
    )
#define mylite_ownerless_lock_table_release_shared(mapping, size, key, owner) \
    mylite_ownerless_lock_table_release_shared( \
        mapping, \
        size, \
        key, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner) \
    )
#define mylite_ownerless_lock_table_release_owner(mapping, size, owner, out_released) \
    mylite_ownerless_lock_table_release_owner( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_released \
    )
#define mylite_ownerless_lock_table_owner_active_count(mapping, size, owner, out_count) \
    mylite_ownerless_lock_table_owner_active_count( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_count \
    )

#define mylite_ownerless_mdl_acquire_shared(table, size, owner, ns, db, object, timeout) \
    mylite_ownerless_mdl_acquire_shared( \
        table, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        ns, \
        db, \
        object, \
        timeout \
    )
#define mylite_ownerless_mdl_acquire_exclusive(table, size, owner, ns, db, object, timeout) \
    mylite_ownerless_mdl_acquire_exclusive( \
        table, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        ns, \
        db, \
        object, \
        timeout \
    )
#define mylite_ownerless_mdl_release_shared(table, size, owner, ns, db, object) \
    mylite_ownerless_mdl_release_shared( \
        table, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        ns, \
        db, \
        object \
    )
#define mylite_ownerless_mdl_release_exclusive(table, size, owner, ns, db, object) \
    mylite_ownerless_mdl_release_exclusive( \
        table, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        ns, \
        db, \
        object \
    )

#define mylite_ownerless_trx_registry_begin(mapping, size, owner, out_id, out_slot, out_gen) \
    mylite_ownerless_trx_registry_begin( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        out_id, \
        out_slot, \
        out_gen \
    )
#define mylite_ownerless_trx_registry_allocate_id(mapping, size, out_id) \
    mylite_ownerless_trx_registry_allocate_id( \
        mapping, \
        size, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_id \
    )
#define mylite_ownerless_trx_registry_ensure_next_id_at_least(mapping, size, min_next) \
    mylite_ownerless_trx_registry_ensure_next_id_at_least( \
        mapping, \
        size, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        min_next \
    )
#define mylite_ownerless_trx_registry_assign_no(mapping, size, trx_id, trx_no) \
    mylite_ownerless_trx_registry_assign_no( \
        mapping, \
        size, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        trx_id, \
        trx_no \
    )
#define mylite_ownerless_trx_registry_assign_new_no(mapping, size, trx_id, out_no) \
    mylite_ownerless_trx_registry_assign_new_no( \
        mapping, \
        size, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        trx_id, \
        out_no \
    )
#define mylite_ownerless_trx_registry_end(mapping, size, slot, gen) \
    mylite_ownerless_trx_registry_end( \
        mapping, \
        size, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        slot, \
        gen \
    )
#define mylite_ownerless_trx_registry_end_by_id(mapping, size, owner, trx_id) \
    mylite_ownerless_trx_registry_end_by_id( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx_id \
    )
#define mylite_ownerless_trx_registry_release_owner(mapping, size, owner, out_released) \
    mylite_ownerless_trx_registry_release_owner( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_released \
    )
#define mylite_ownerless_trx_registry_snapshot(mapping, size, ids, cap, count, next, oldest) \
    mylite_ownerless_trx_registry_snapshot( \
        mapping, \
        size, \
        ids, \
        cap, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        count, \
        next, \
        oldest \
    )
#define mylite_ownerless_trx_registry_snapshot_read_view(mapping, size, ids, cap, count, next, min_no) \
    mylite_ownerless_trx_registry_snapshot_read_view( \
        mapping, \
        size, \
        ids, \
        cap, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        count, \
        next, \
        min_no \
    )
#define mylite_ownerless_trx_registry_owner_active_count(mapping, size, owner, out_count) \
    mylite_ownerless_trx_registry_owner_active_count( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_count \
    )

#define mylite_ownerless_read_view_registry_open(mapping, size, owner, low_id, low_no, ids, count, out_slot, out_gen) \
    mylite_ownerless_read_view_registry_open( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        low_id, \
        low_no, \
        ids, \
        count, \
        out_slot, \
        out_gen \
    )
#define mylite_ownerless_read_view_registry_close(mapping, size, owner, slot, gen) \
    mylite_ownerless_read_view_registry_close( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        slot, \
        gen \
    )
#define mylite_ownerless_read_view_registry_release_owner(mapping, size, owner, out_released) \
    mylite_ownerless_read_view_registry_release_owner( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_released \
    )
#define mylite_ownerless_read_view_registry_snapshot_oldest(mapping, size, ids, cap, count, low_id, low_no) \
    mylite_ownerless_read_view_registry_snapshot_oldest( \
        mapping, \
        size, \
        ids, \
        cap, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        count, \
        low_id, \
        low_no \
    )
#define mylite_ownerless_read_view_registry_owner_active_count(mapping, size, owner, out_count) \
    mylite_ownerless_read_view_registry_owner_active_count( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_count \
    )

#define mylite_ownerless_innodb_lock_registry_acquire_table(mapping, size, owner, trx, table, mode, timeout) \
    mylite_ownerless_innodb_lock_registry_acquire_table( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        table, \
        mode, \
        timeout \
    )
#define mylite_ownerless_innodb_lock_registry_reserve_table(mapping, size, owner, trx, table, mode, timeout) \
    mylite_ownerless_innodb_lock_registry_reserve_table( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        table, \
        mode, \
        timeout \
    )
#define mylite_ownerless_innodb_lock_registry_release_table(mapping, size, owner, trx, table, mode) \
    mylite_ownerless_innodb_lock_registry_release_table( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        table, \
        mode \
    )
#define mylite_ownerless_innodb_lock_registry_wait_for_table(mapping, size, owner, trx, table, mode, blocker_owner, blocker_trx) \
    mylite_ownerless_innodb_lock_registry_wait_for_table( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        table, \
        mode, \
        blocker_owner, \
        blocker_trx \
    )
#define mylite_ownerless_innodb_lock_registry_wait_until_table_available(mapping, size, owner, trx, table, mode, timeout) \
    mylite_ownerless_innodb_lock_registry_wait_until_table_available( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        table, \
        mode, \
        timeout \
    )
#define mylite_ownerless_innodb_lock_registry_acquire_record(mapping, size, owner, trx, index_id, space, page, heap, mode, flags, timeout) \
    mylite_ownerless_innodb_lock_registry_acquire_record( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        index_id, \
        space, \
        page, \
        heap, \
        mode, \
        flags, \
        timeout \
    )
#define mylite_ownerless_innodb_lock_registry_reserve_record(mapping, size, owner, trx, index_id, space, page, heap, mode, flags, timeout) \
    mylite_ownerless_innodb_lock_registry_reserve_record( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        index_id, \
        space, \
        page, \
        heap, \
        mode, \
        flags, \
        timeout \
    )
#define mylite_ownerless_innodb_lock_registry_release_record(mapping, size, owner, trx, index_id, space, page, heap, mode, flags) \
    mylite_ownerless_innodb_lock_registry_release_record( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        index_id, \
        space, \
        page, \
        heap, \
        mode, \
        flags \
    )
#define mylite_ownerless_innodb_lock_registry_wait_for_record(mapping, size, owner, trx, index_id, space, page, heap, mode, flags, blocker_owner, blocker_trx) \
    mylite_ownerless_innodb_lock_registry_wait_for_record( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        index_id, \
        space, \
        page, \
        heap, \
        mode, \
        flags, \
        blocker_owner, \
        blocker_trx \
    )
#define mylite_ownerless_innodb_lock_registry_wait_until_record_available(mapping, size, owner, trx, index_id, space, page, heap, mode, flags, timeout) \
    mylite_ownerless_innodb_lock_registry_wait_until_record_available( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        index_id, \
        space, \
        page, \
        heap, \
        mode, \
        flags, \
        timeout \
    )
#define mylite_ownerless_innodb_lock_registry_clear_wait(mapping, size, owner, trx, out_cleared) \
    mylite_ownerless_innodb_lock_registry_clear_wait( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_OWNER_GENERATION(owner), \
        trx, \
        out_cleared \
    )
#define mylite_ownerless_innodb_lock_registry_release_owner(mapping, size, owner, out_released) \
    mylite_ownerless_innodb_lock_registry_release_owner( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_released \
    )
#define mylite_ownerless_innodb_lock_registry_owner_active_count(mapping, size, owner, out_count) \
    mylite_ownerless_innodb_lock_registry_owner_active_count( \
        mapping, \
        size, \
        owner, \
        MYLITE_TEST_LATCH_OWNER_ID, \
        MYLITE_TEST_LATCH_OWNER_GENERATION, \
        out_count \
    )

#endif
