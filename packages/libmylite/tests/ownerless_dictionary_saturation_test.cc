#include "ownerless_dictionary_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t k_generation_offset = 0U;
constexpr std::size_t k_active_owner_id_offset = 8U;
constexpr std::size_t k_wake_word_offset = 12U;
constexpr std::size_t k_active_owner_generation_offset = 16U;
constexpr std::size_t k_active_owner_pid_offset = 24U;
constexpr std::size_t k_active_owner_start_time_offset = 32U;
constexpr std::size_t k_active_owner_boot_id_hash_offset = 40U;
constexpr std::size_t k_recoverable_kind_offset = 48U;
constexpr std::size_t k_recoverable_owner_id_offset = 52U;
constexpr std::size_t k_recoverable_owner_generation_offset = 56U;

constexpr std::uint32_t k_owner_id = 7U;
constexpr std::uint64_t k_owner_generation = 19U;
constexpr std::uint64_t k_output_sentinel = 0x123456789abcdef0ULL;
constexpr std::uint64_t k_last_ready_generation = std::numeric_limits<std::uint64_t>::max() - 1U;
constexpr std::uint64_t k_last_active_generation = std::numeric_limits<std::uint64_t>::max() - 2U;

using DictionaryState = std::array<unsigned char, MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE>;

[[noreturn]] void fail(const char *expression, int line) {
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::abort();
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fail(#expression, __LINE__);                                                           \
        }                                                                                          \
    } while (false)

void store32(DictionaryState *state, std::size_t offset, std::uint32_t value) {
    CHECK(state != nullptr);
    CHECK(offset + sizeof(value) <= state->size());
    std::memcpy(state->data() + offset, &value, sizeof(value));
}

void store64(DictionaryState *state, std::size_t offset, std::uint64_t value) {
    CHECK(state != nullptr);
    CHECK(offset + sizeof(value) <= state->size());
    std::memcpy(state->data() + offset, &value, sizeof(value));
}

std::uint64_t load64(const DictionaryState &state, std::size_t offset) {
    std::uint64_t value = 0U;
    CHECK(offset + sizeof(value) <= state.size());
    std::memcpy(&value, state.data() + offset, sizeof(value));
    return value;
}

mylite_ownerless_process_identity owner_identity() {
    mylite_ownerless_process_identity identity = {};
    identity.pid = 101U;
    identity.start_time = 202U;
    identity.boot_id_hash = 303U;
    return identity;
}

void seed_active(DictionaryState *state, std::uint64_t generation, bool recoverable) {
    CHECK(
        mylite_ownerless_dictionary_state_initialize(state->data(), state->size()) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    const mylite_ownerless_process_identity identity = owner_identity();
    store64(state, k_generation_offset, generation);
    store32(state, k_active_owner_id_offset, k_owner_id);
    store32(state, k_wake_word_offset, 41U);
    store64(state, k_active_owner_generation_offset, k_owner_generation);
    store64(state, k_active_owner_pid_offset, identity.pid);
    store64(state, k_active_owner_start_time_offset, identity.start_time);
    store64(state, k_active_owner_boot_id_hash_offset, identity.boot_id_hash);
    if (recoverable) {
        store32(
            state,
            k_recoverable_kind_offset,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE
        );
        store32(state, k_recoverable_owner_id_offset, k_owner_id);
        store64(state, k_recoverable_owner_generation_offset, k_owner_generation);
    }
}

void check_unchanged(
    const DictionaryState &state,
    const DictionaryState &before,
    std::uint64_t output
) {
    CHECK(state == before);
    CHECK(output == k_output_sentinel);
}

void test_begin_rejects_last_ready_generation_without_claiming_owner() {
    alignas(8) DictionaryState state = {};
    CHECK(
        mylite_ownerless_dictionary_state_initialize(state.data(), state.size()) ==
        MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    store64(&state, k_generation_offset, k_last_ready_generation);
    const DictionaryState before = state;
    std::uint64_t output = k_output_sentinel;

    CHECK(
        mylite_ownerless_dictionary_state_begin_ddl(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            owner_identity(),
            1U,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    check_unchanged(state, before, output);

    CHECK(
        mylite_ownerless_dictionary_state_begin_ddl(
            state.data(),
            state.size(),
            k_owner_id + 1U,
            k_owner_generation + 1U,
            owner_identity(),
            1U,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    check_unchanged(state, before, output);
}

void test_last_active_generation_finishes_without_wrap_or_reuse() {
    alignas(8) DictionaryState state = {};
    seed_active(&state, k_last_active_generation, false);
    std::uint64_t output = k_output_sentinel;

    CHECK(
        mylite_ownerless_dictionary_state_finish_ddl(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    CHECK(output == k_last_ready_generation);
    CHECK(load64(state, k_generation_offset) == k_last_ready_generation);

    const DictionaryState before = state;
    output = k_output_sentinel;
    CHECK(
        mylite_ownerless_dictionary_state_begin_ddl(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            owner_identity(),
            1U,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    check_unchanged(state, before, output);
}

void test_finish_rejects_max_generation_without_clearing_owner() {
    alignas(8) DictionaryState state = {};
    seed_active(&state, std::numeric_limits<std::uint64_t>::max(), true);
    const DictionaryState before = state;
    std::uint64_t output = k_output_sentinel;

    CHECK(
        mylite_ownerless_dictionary_state_finish_ddl(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    check_unchanged(state, before, output);
}

void test_recovery_rejects_max_generation_without_clearing_owner() {
    alignas(8) DictionaryState state = {};
    seed_active(&state, std::numeric_limits<std::uint64_t>::max(), true);
    const DictionaryState before = state;
    std::uint64_t output = k_output_sentinel;

    CHECK(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    check_unchanged(state, before, output);
}

void test_last_recovery_generation_completes_once_without_aba() {
    alignas(8) DictionaryState state = {};
    seed_active(&state, k_last_active_generation, true);
    std::uint64_t output = k_output_sentinel;

    CHECK(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK
    );
    CHECK(output == k_last_ready_generation);
    CHECK(load64(state, k_generation_offset) == k_last_ready_generation);

    const DictionaryState before = state;
    output = k_output_sentinel;
    CHECK(
        mylite_ownerless_dictionary_state_recover_dead_owner(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_ERROR
    );
    check_unchanged(state, before, output);

    CHECK(
        mylite_ownerless_dictionary_state_begin_ddl(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            owner_identity(),
            1U,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    check_unchanged(state, before, output);
}

void test_mark_recoverable_rejects_max_generation_without_mutation() {
    alignas(8) DictionaryState state = {};
    seed_active(&state, std::numeric_limits<std::uint64_t>::max(), false);
    const DictionaryState before = state;

    CHECK(
        mylite_ownerless_dictionary_state_mark_recoverable(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    CHECK(state == before);
}

void test_incomplete_recovery_rejects_saturated_state_without_clear() {
    alignas(8) DictionaryState state = {};
    seed_active(&state, k_last_ready_generation, true);
    const DictionaryState before = state;
    std::uint64_t output = k_output_sentinel;

    CHECK(
        mylite_ownerless_dictionary_state_recover_incomplete_owner(
            state.data(),
            state.size(),
            k_owner_id,
            k_owner_generation,
            &output
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_EXHAUSTED
    );
    check_unchanged(state, before, output);
}

} // namespace

int main() {
    test_begin_rejects_last_ready_generation_without_claiming_owner();
    test_last_active_generation_finishes_without_wrap_or_reuse();
    test_finish_rejects_max_generation_without_clearing_owner();
    test_recovery_rejects_max_generation_without_clearing_owner();
    test_last_recovery_generation_completes_once_without_aba();
    test_mark_recoverable_rejects_max_generation_without_mutation();
    test_incomplete_recovery_rejects_saturated_state_without_clear();
    return 0;
}
