#include "ownerless_latch.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

static_assert(sizeof(mylite_ownerless_latch) == MYLITE_OWNERLESS_LATCH_SIZE);

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms);
unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline);
std::uint32_t load32(const std::uint32_t *value);
std::uint64_t load64(const std::uint64_t *value);
void store64(std::uint64_t *target, std::uint64_t value);
bool cas64(std::uint64_t *target, std::uint64_t *expected, std::uint64_t value);
int acquire_latch(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    unsigned timeout_ms,
    bool recoverable,
    mylite_ownerless_latch_dead_owner *out_dead_owner
);
std::uint64_t latch_state_owner(std::uint32_t state, std::uint32_t owner_id);
std::uint32_t latch_state(std::uint64_t state_owner);
std::uint32_t latch_owner_id(std::uint64_t state_owner);
bool latch_owner_dead(
    const mylite_ownerless_latch *latch,
    std::uint64_t state_owner,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    std::uint64_t *out_owner_generation
);
bool latch_state_is_owned(std::uint32_t state);
bool consume_test_release_pending();
void wake_latch_waiters(mylite_ownerless_latch *latch);

std::uint32_t test_release_pending_count = 0U;

} // namespace

void mylite_ownerless_latch_initialize(mylite_ownerless_latch *latch) {
    if (latch == nullptr) {
        return;
    }
    std::memset(latch, 0, sizeof(*latch));
}

int mylite_ownerless_latch_acquire(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    unsigned timeout_ms
) {
    return acquire_latch(
        latch,
        owner_id,
        owner_generation,
        is_owner_alive,
        owner_alive_ctx,
        timeout_ms,
        false,
        nullptr
    );
}

int mylite_ownerless_latch_acquire_recoverable(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    unsigned timeout_ms,
    mylite_ownerless_latch_dead_owner *out_dead_owner
) {
    if (out_dead_owner == nullptr || is_owner_alive == nullptr) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }
    std::memset(out_dead_owner, 0, sizeof(*out_dead_owner));
    return acquire_latch(
        latch,
        owner_id,
        owner_generation,
        is_owner_alive,
        owner_alive_ctx,
        timeout_ms,
        true,
        out_dead_owner
    );
}

int mylite_ownerless_latch_mark_consistent(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (latch == nullptr || owner_id == 0U || owner_generation == 0U ||
        load64(&latch->state_owner) !=
            latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RECOVERING, owner_id) ||
        load64(&latch->owner_generation) != owner_generation) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }

    std::uint64_t expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RECOVERING, owner_id);
    if (!cas64(
            &latch->state_owner,
            &expected,
            latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_LOCKED, owner_id)
        )) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }
    wake_latch_waiters(latch);
    return MYLITE_OWNERLESS_LATCH_OK;
}

int mylite_ownerless_latch_mark_not_recoverable(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (latch == nullptr || owner_id == 0U || owner_generation == 0U ||
        load64(&latch->state_owner) !=
            latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RECOVERING, owner_id) ||
        load64(&latch->owner_generation) != owner_generation) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }

    std::uint64_t expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RECOVERING, owner_id);
    if (!cas64(
            &latch->state_owner,
            &expected,
            latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_OWNER_DEAD, owner_id)
        )) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }
    wake_latch_waiters(latch);
    return MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE;
}

namespace {

int acquire_latch(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    unsigned timeout_ms,
    bool recoverable,
    mylite_ownerless_latch_dead_owner *out_dead_owner
) {
    if (latch == nullptr || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }

    const auto deadline = wait_deadline(timeout_ms);
    for (;;) {
        std::uint64_t observed = load64(&latch->state_owner);
        if (observed == latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED, 0U)) {
            std::uint64_t expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED, 0U);
            if (cas64(
                    &latch->state_owner,
                    &expected,
                    latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING, owner_id)
                )) {
                store64(&latch->owner_generation, owner_generation);
                expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING, owner_id);
                if (load64(&latch->owner_generation) != owner_generation ||
                    !cas64(
                        &latch->state_owner,
                        &expected,
                        latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_LOCKED, owner_id)
                    )) {
                    return MYLITE_OWNERLESS_LATCH_ERROR;
                }
                wake_latch_waiters(latch);
                return MYLITE_OWNERLESS_LATCH_OK;
            }
            continue;
        }
        const std::uint32_t observed_state = latch_state(observed);
        if (observed_state == MYLITE_OWNERLESS_LATCH_STATE_OWNER_DEAD) {
            return recoverable ? MYLITE_OWNERLESS_LATCH_NOT_RECOVERABLE
                               : MYLITE_OWNERLESS_LATCH_OWNER_DEAD;
        }
        if (!latch_state_is_owned(observed_state)) {
            return MYLITE_OWNERLESS_LATCH_ERROR;
        }
        if (observed_state == MYLITE_OWNERLESS_LATCH_STATE_RELEASE_PENDING &&
            latch_owner_id(observed) == owner_id &&
            load64(&latch->owner_generation) == owner_generation) {
            std::uint64_t expected = observed;
            if (cas64(
                    &latch->state_owner,
                    &expected,
                    latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_LOCKED, owner_id)
                )) {
                return MYLITE_OWNERLESS_LATCH_OK;
            }
            continue;
        }
        std::uint64_t dead_owner_generation = 0U;
        if (latch_owner_dead(
                latch,
                observed,
                is_owner_alive,
                owner_alive_ctx,
                &dead_owner_generation
            )) {
            __atomic_add_fetch(&latch->owner_death_count, 1U, __ATOMIC_ACQ_REL);
            if (!recoverable) {
                return MYLITE_OWNERLESS_LATCH_OWNER_DEAD;
            }

            std::uint64_t expected = observed;
            const std::uint32_t claim_state =
                observed_state == MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING
                    ? MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING
                    : MYLITE_OWNERLESS_LATCH_STATE_CLAIMING_RECOVERY;
            if (!cas64(&latch->state_owner, &expected, latch_state_owner(claim_state, owner_id))) {
                continue;
            }
            store64(&latch->owner_generation, owner_generation);
            if (observed_state == MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING) {
                /* The previous owner had not entered the protected mutation. */
                expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING, owner_id);
                if (load64(&latch->owner_generation) != owner_generation ||
                    !cas64(
                        &latch->state_owner,
                        &expected,
                        latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_LOCKED, owner_id)
                    )) {
                    return MYLITE_OWNERLESS_LATCH_ERROR;
                }
                wake_latch_waiters(latch);
                return MYLITE_OWNERLESS_LATCH_OK;
            }

            expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_CLAIMING_RECOVERY, owner_id);
            if (load64(&latch->owner_generation) != owner_generation ||
                !cas64(
                    &latch->state_owner,
                    &expected,
                    latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RECOVERING, owner_id)
                )) {
                return MYLITE_OWNERLESS_LATCH_ERROR;
            }
            out_dead_owner->owner_id = latch_owner_id(observed);
            out_dead_owner->owner_generation = dead_owner_generation;
            wake_latch_waiters(latch);
            return MYLITE_OWNERLESS_LATCH_RECOVERY_REQUIRED;
        }

        const std::uint32_t wait_epoch = mylite_ownerless_wait_load(&latch->wake_epoch);
        __atomic_add_fetch(&latch->waiter_count, 1U, __ATOMIC_ACQ_REL);
        const unsigned timeout_ms_remaining = remaining_timeout_ms(deadline);
        if (timeout_ms_remaining == 0U) {
            __atomic_sub_fetch(&latch->waiter_count, 1U, __ATOMIC_ACQ_REL);
            return MYLITE_OWNERLESS_LATCH_TIMEOUT;
        }
        const int wait_result =
            mylite_ownerless_wait_for_change(&latch->wake_epoch, wait_epoch, timeout_ms_remaining);
        __atomic_sub_fetch(&latch->waiter_count, 1U, __ATOMIC_ACQ_REL);
        if (wait_result == MYLITE_OWNERLESS_WAIT_TIMEOUT) {
            if (load64(&latch->state_owner) != observed) {
                continue;
            }
            return MYLITE_OWNERLESS_LATCH_TIMEOUT;
        }
        if (wait_result != MYLITE_OWNERLESS_WAIT_OK) {
            return MYLITE_OWNERLESS_LATCH_ERROR;
        }
    }
}

} // namespace

int mylite_ownerless_latch_release(
    mylite_ownerless_latch *latch,
    std::uint32_t owner_id,
    std::uint64_t owner_generation
) {
    if (latch == nullptr || owner_generation == 0U ||
        load64(&latch->state_owner) !=
            latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_LOCKED, owner_id) ||
        load64(&latch->owner_generation) != owner_generation) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }

    std::uint64_t expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_LOCKED, owner_id);
    if (!cas64(
            &latch->state_owner,
            &expected,
            latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RELEASING, owner_id)
        )) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }
    if (consume_test_release_pending()) {
        expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RELEASING, owner_id);
        if (!cas64(
                &latch->state_owner,
                &expected,
                latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RELEASE_PENDING, owner_id)
            )) {
            return MYLITE_OWNERLESS_LATCH_ERROR;
        }
        wake_latch_waiters(latch);
        return MYLITE_OWNERLESS_LATCH_RELEASE_PENDING;
    }
    /* Unlocked is published last so a claimant never inherits an old generation. */
    store64(&latch->owner_generation, 0U);
    expected = latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_RELEASING, owner_id);
    if (!cas64(
            &latch->state_owner,
            &expected,
            latch_state_owner(MYLITE_OWNERLESS_LATCH_STATE_UNLOCKED, 0U)
        )) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }
    wake_latch_waiters(latch);
    return MYLITE_OWNERLESS_LATCH_OK;
}

void mylite_ownerless_latch_test_inject_release_pending_once(void) {
    __atomic_add_fetch(&test_release_pending_count, 1U, __ATOMIC_RELEASE);
}

int mylite_ownerless_latch_snapshot(
    const mylite_ownerless_latch *latch,
    std::uint32_t *out_state,
    std::uint32_t *out_owner_id,
    std::uint64_t *out_owner_generation,
    std::uint32_t *out_waiter_count,
    std::uint64_t *out_owner_death_count
) {
    if (latch == nullptr || out_state == nullptr || out_owner_id == nullptr ||
        out_owner_generation == nullptr || out_waiter_count == nullptr ||
        out_owner_death_count == nullptr) {
        return MYLITE_OWNERLESS_LATCH_ERROR;
    }

    const std::uint64_t state_owner = load64(&latch->state_owner);
    *out_state = latch_state(state_owner);
    *out_owner_id = latch_owner_id(state_owner);
    *out_owner_generation = load64(&latch->owner_generation);
    *out_waiter_count = load32(&latch->waiter_count);
    *out_owner_death_count = load64(&latch->owner_death_count);
    return MYLITE_OWNERLESS_LATCH_OK;
}

namespace {

std::chrono::steady_clock::time_point wait_deadline(unsigned timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

unsigned remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<unsigned>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 1));
}

std::uint32_t load32(const std::uint32_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

std::uint64_t load64(const std::uint64_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void store64(std::uint64_t *target, std::uint64_t value) {
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

bool cas64(std::uint64_t *target, std::uint64_t *expected, std::uint64_t value) {
    return __atomic_compare_exchange_n(
        target,
        expected,
        value,
        false,
        __ATOMIC_ACQ_REL,
        __ATOMIC_RELAXED
    );
}

bool consume_test_release_pending() {
    std::uint32_t count = __atomic_load_n(&test_release_pending_count, __ATOMIC_ACQUIRE);
    while (count != 0U) {
        if (__atomic_compare_exchange_n(
                &test_release_pending_count,
                &count,
                count - 1U,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            return true;
        }
    }
    return false;
}

std::uint64_t latch_state_owner(std::uint32_t state, std::uint32_t owner_id) {
    return (static_cast<std::uint64_t>(owner_id) << 32U) | state;
}

std::uint32_t latch_state(std::uint64_t state_owner) {
    return static_cast<std::uint32_t>(state_owner & 0xffffffffULL);
}

std::uint32_t latch_owner_id(std::uint64_t state_owner) {
    return static_cast<std::uint32_t>(state_owner >> 32U);
}

bool latch_owner_dead(
    const mylite_ownerless_latch *latch,
    std::uint64_t state_owner,
    mylite_ownerless_latch_owner_alive_callback is_owner_alive,
    void *owner_alive_ctx,
    std::uint64_t *out_owner_generation
) {
    if (is_owner_alive == nullptr) {
        return false;
    }
    const std::uint32_t owner_id = latch_owner_id(state_owner);
    const std::uint64_t owner_generation = load64(&latch->owner_generation);
    if (owner_id == 0U || load64(&latch->state_owner) != state_owner) {
        return false;
    }
    const int alive = is_owner_alive(owner_id, owner_generation, owner_alive_ctx);
    if (load64(&latch->state_owner) != state_owner ||
        load64(&latch->owner_generation) != owner_generation || alive != 0) {
        return false;
    }
    if (out_owner_generation != nullptr) {
        *out_owner_generation = owner_generation;
    }
    return true;
}

bool latch_state_is_owned(std::uint32_t state) {
    return state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED ||
           state == MYLITE_OWNERLESS_LATCH_STATE_ACQUIRING ||
           state == MYLITE_OWNERLESS_LATCH_STATE_RECOVERING ||
           state == MYLITE_OWNERLESS_LATCH_STATE_CLAIMING_RECOVERY ||
           state == MYLITE_OWNERLESS_LATCH_STATE_RELEASING ||
           state == MYLITE_OWNERLESS_LATCH_STATE_RELEASE_PENDING;
}

void wake_latch_waiters(mylite_ownerless_latch *latch) {
    mylite_ownerless_wait_store(
        &latch->wake_epoch,
        mylite_ownerless_wait_load(&latch->wake_epoch) + 1U
    );
    static_cast<void>(mylite_ownerless_wait_wake(&latch->wake_epoch));
}

} // namespace
