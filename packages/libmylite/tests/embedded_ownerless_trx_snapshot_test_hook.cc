#include "trx0sys.h"

#include <cstdint>

extern "C" int mylite_test_ownerless_snapshot_ids(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_low_limit_id,
    uint64_t *out_low_limit_no
) {
    trx_ids_t ids;
    trx_id_t low_limit_id = 0;
    trx_id_t low_limit_no = 0;
    const dberr_t error = trx_sys.snapshot_ids(nullptr, &ids, &low_limit_id, &low_limit_no);
    if (error != DB_SUCCESS)
        return 1;

    *out_trx_id_count = static_cast<unsigned int>(ids.size());
    *out_low_limit_id = static_cast<uint64_t>(low_limit_id);
    *out_low_limit_no = static_cast<uint64_t>(low_limit_no);
    if (ids.size() > trx_id_capacity)
        return 2;

    for (size_t i = 0; i < ids.size(); ++i)
        out_trx_ids[i] = static_cast<uint64_t>(ids[i]);
    return 0;
}
