#include "ownerless_process_registry.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::uint32_t k_slot_count = 4U;
constexpr std::size_t k_header_generation_offset = 8U;
constexpr std::size_t k_slot_open_mode_offset = 12U;

void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "ownerless process mode test failed: %s\n", message);
        std::abort();
    }
}

mylite_ownerless_process_identity current_identity() {
    mylite_ownerless_process_identity identity = {};
    check(
        mylite_ownerless_current_process_identity(&identity) ==
            MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "read current process identity"
    );
    return identity;
}

mylite_ownerless_process_identity distinct_identity(
    const mylite_ownerless_process_identity &identity,
    std::uint64_t discriminator
) {
    mylite_ownerless_process_identity distinct = identity;
    distinct.start_time += discriminator;
    check(distinct.start_time != 0U, "construct distinct process identity");
    return distinct;
}

std::vector<unsigned char> initialized_registry() {
    const std::size_t size = mylite_ownerless_process_registry_size(k_slot_count);
    check(size != 0U, "compute registry size");
    std::vector<unsigned char> registry(size);
    check(
        mylite_ownerless_process_registry_initialize(
            registry.data(),
            registry.size(),
            k_slot_count
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "initialize registry"
    );
    return registry;
}

int allocate(
    std::vector<unsigned char> &registry,
    const mylite_ownerless_process_identity &identity,
    std::uint32_t open_mode,
    std::uint32_t *out_slot,
    std::uint64_t *out_generation
) {
    return mylite_ownerless_process_registry_allocate(
        registry.data(),
        registry.size(),
        identity,
        open_mode,
        1U,
        out_slot,
        out_generation
    );
}

void release(std::vector<unsigned char> &registry, std::uint32_t slot, std::uint64_t generation) {
    check(
        mylite_ownerless_process_registry_release(
            registry.data(),
            registry.size(),
            slot,
            generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "release process slot"
    );
}

int always_dead(const mylite_ownerless_process_identity *identity, void *context) {
    (void)identity;
    (void)context;
    return 0;
}

bool write_all(int fd, const void *buffer, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    std::size_t written = 0U;
    while (written < size) {
        const ssize_t result = ::write(fd, bytes + written, size - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool read_all(int fd, void *buffer, std::size_t size) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    std::size_t read_size = 0U;
    while (read_size < size) {
        const ssize_t result = ::read(fd, bytes + read_size, size - read_size);
        if (result > 0) {
            read_size += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void test_ordinary_exclusive_blocks_every_active_mode() {
    auto registry = initialized_registry();
    const auto identity = current_identity();
    std::uint32_t ordinary_slot = 0U;
    std::uint64_t ordinary_generation = 0U;
    check(
        allocate(
            registry,
            identity,
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE,
            &ordinary_slot,
            &ordinary_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "allocate ordinary-exclusive slot"
    );

    for (const std::uint32_t mode : {
             MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE,
             MYLITE_OWNERLESS_PROCESS_OPEN_MODE_SHARED_READONLY,
             MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW,
         }) {
        std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
        std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
        check(
            allocate(registry, distinct_identity(identity, mode), mode, &slot, &generation) ==
                MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY,
            "reject mode while ordinary-exclusive slot is active"
        );
        check(slot == 0U && generation == 0U, "clear outputs on incompatible allocation");
    }

    check(
        mylite_ownerless_process_registry_active_count(registry.data()) == 1U,
        "retain only ordinary-exclusive slot"
    );
    release(registry, ordinary_slot, ordinary_generation);
}

void test_ownerless_modes_are_mutually_compatible() {
    auto registry = initialized_registry();
    const auto identity = current_identity();
    std::uint32_t ownerless_slot = 0U;
    std::uint64_t ownerless_generation = 0U;
    std::uint32_t readonly_slot = 0U;
    std::uint64_t readonly_generation = 0U;
    std::uint32_t second_ownerless_slot = 0U;
    std::uint64_t second_ownerless_generation = 0U;

    check(
        allocate(
            registry,
            identity,
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW,
            &ownerless_slot,
            &ownerless_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "allocate ownerless-RW slot"
    );
    check(
        allocate(
            registry,
            distinct_identity(identity, 1U),
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_SHARED_READONLY,
            &readonly_slot,
            &readonly_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "allocate shared-readonly beside ownerless-RW"
    );
    check(
        allocate(
            registry,
            distinct_identity(identity, 2U),
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW,
            &second_ownerless_slot,
            &second_ownerless_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "allocate second ownerless-RW slot"
    );

    std::uint32_t ordinary_slot = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t ordinary_generation = std::numeric_limits<std::uint64_t>::max();
    check(
        allocate(
            registry,
            distinct_identity(identity, 3U),
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE,
            &ordinary_slot,
            &ordinary_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY,
        "reject ordinary-exclusive beside ownerless modes"
    );
    check(
        ordinary_slot == 0U && ordinary_generation == 0U,
        "clear outputs for blocked ordinary-exclusive allocation"
    );
    check(
        mylite_ownerless_process_registry_active_count(registry.data()) == 3U,
        "retain all compatible ownerless slots"
    );

    release(registry, second_ownerless_slot, second_ownerless_generation);
    release(registry, readonly_slot, readonly_generation);
    release(registry, ownerless_slot, ownerless_generation);
}

void test_incompatible_allocations_are_serialized() {
    struct allocation_result {
        int status;
        std::uint32_t slot;
        std::uint64_t generation;
    };

    const std::size_t registry_size = mylite_ownerless_process_registry_size(k_slot_count);
    void *mapping =
        ::mmap(nullptr, registry_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check(mapping != MAP_FAILED, "map shared process registry");
    check(
        mylite_ownerless_process_registry_initialize(mapping, registry_size, k_slot_count) ==
            MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "initialize shared process registry"
    );

    int start_pipe[2] = {-1, -1};
    int result_pipe[2] = {-1, -1};
    check(::pipe(start_pipe) == 0, "create allocation start pipe");
    check(::pipe(result_pipe) == 0, "create allocation result pipe");

    const pid_t child = ::fork();
    check(child >= 0, "fork ownerless allocator");
    if (child == 0) {
        static_cast<void>(::close(start_pipe[1]));
        static_cast<void>(::close(result_pipe[0]));
        unsigned char start = 0U;
        allocation_result child_result = {
            MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR,
            0U,
            0U,
        };
        mylite_ownerless_process_identity identity = {};
        if (!read_all(start_pipe[0], &start, sizeof(start)) ||
            mylite_ownerless_current_process_identity(&identity) !=
                MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
            _exit(2);
        }
        child_result.status = mylite_ownerless_process_registry_allocate(
            mapping,
            registry_size,
            identity,
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW,
            1U,
            &child_result.slot,
            &child_result.generation
        );
        if (!write_all(result_pipe[1], &child_result, sizeof(child_result))) {
            _exit(3);
        }
        _exit(0);
    }

    static_cast<void>(::close(start_pipe[0]));
    static_cast<void>(::close(result_pipe[1]));
    const unsigned char start = 1U;
    check(write_all(start_pipe[1], &start, sizeof(start)), "start child allocation");

    allocation_result parent_result = {
        MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR,
        0U,
        0U,
    };
    parent_result.status = mylite_ownerless_process_registry_allocate(
        mapping,
        registry_size,
        current_identity(),
        MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE,
        1U,
        &parent_result.slot,
        &parent_result.generation
    );

    allocation_result child_result = {
        MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR,
        0U,
        0U,
    };
    check(
        read_all(result_pipe[0], &child_result, sizeof(child_result)),
        "read child allocation result"
    );
    int child_status = 0;
    check(::waitpid(child, &child_status, 0) == child, "wait for ownerless allocator");
    check(
        WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
        "child allocator exits cleanly"
    );

    const allocation_result *winner = parent_result.status == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK
                                          ? &parent_result
                                          : &child_result;
    const allocation_result *blocked = winner == &parent_result ? &child_result : &parent_result;
    check(
        winner->status == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "one incompatible racing allocation succeeds"
    );
    check(
        blocked->status == MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY,
        "one incompatible racing allocation returns busy"
    );
    check(
        blocked->slot == 0U && blocked->generation == 0U,
        "blocked racing allocation has no slot token"
    );
    check(
        mylite_ownerless_process_registry_active_count(mapping) == 1U,
        "racing incompatible allocations publish one slot"
    );
    check(
        mylite_ownerless_process_registry_release(
            mapping,
            registry_size,
            winner->slot,
            winner->generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "release winning racing allocation"
    );

    static_cast<void>(::close(start_pipe[1]));
    static_cast<void>(::close(result_pipe[0]));
    check(::munmap(mapping, registry_size) == 0, "unmap shared process registry");
}

void test_active_stale_slot_remains_authoritative_until_cleanup() {
    auto registry = initialized_registry();
    const auto identity = current_identity();
    const auto stale_identity = distinct_identity(identity, 100U);
    std::uint32_t stale_slot = 0U;
    std::uint64_t stale_generation = 0U;
    check(
        allocate(
            registry,
            stale_identity,
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW,
            &stale_slot,
            &stale_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "allocate stale ownerless slot"
    );

    std::uint32_t ordinary_slot = 0U;
    std::uint64_t ordinary_generation = 0U;
    check(
        allocate(
            registry,
            identity,
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE,
            &ordinary_slot,
            &ordinary_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY,
        "treat active stale slot as authoritative"
    );

    std::uint32_t cleaned_slots = 0U;
    check(
        mylite_ownerless_process_registry_cleanup_dead(
            registry.data(),
            registry.size(),
            always_dead,
            nullptr,
            &cleaned_slots
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "explicitly clean stale slot"
    );
    check(cleaned_slots == 1U, "clean exactly one stale slot");
    check(
        allocate(
            registry,
            identity,
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE,
            &ordinary_slot,
            &ordinary_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "allocate ordinary-exclusive after stale cleanup"
    );
    release(registry, ordinary_slot, ordinary_generation);
}

void test_invalid_modes_fail_closed() {
    auto registry = initialized_registry();
    const auto identity = current_identity();

    for (const std::uint32_t mode : {0U, 4U, std::numeric_limits<std::uint32_t>::max()}) {
        std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
        std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
        check(
            allocate(registry, identity, mode, &slot, &generation) ==
                MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR,
            "reject invalid requested mode"
        );
        check(slot == 0U && generation == 0U, "clear outputs for invalid requested mode");
    }

    std::uint32_t slot = 0U;
    std::uint64_t generation = 0U;
    check(
        allocate(
            registry,
            identity,
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK,
        "allocate slot before persisted-mode corruption"
    );
    std::uint32_t invalid_mode = 4U;
    std::memcpy(
        registry.data() + MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE + k_slot_open_mode_offset,
        &invalid_mode,
        sizeof(invalid_mode)
    );

    std::uint32_t next_slot = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t next_generation = std::numeric_limits<std::uint64_t>::max();
    check(
        allocate(
            registry,
            distinct_identity(identity, 1U),
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_SHARED_READONLY,
            &next_slot,
            &next_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR,
        "fail closed for invalid active slot mode"
    );
    check(next_slot == 0U && next_generation == 0U, "clear outputs after persisted-mode error");
    check(
        mylite_ownerless_process_registry_active_count(registry.data()) == 1U,
        "do not mutate invalid active slot"
    );

    next_slot = std::numeric_limits<std::uint32_t>::max();
    next_generation = std::numeric_limits<std::uint64_t>::max();
    check(
        allocate(
            registry,
            distinct_identity(identity, 2U),
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_ORDINARY_EXCLUSIVE,
            &next_slot,
            &next_generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY,
        "block ordinary-exclusive on any active slot"
    );
    check(
        next_slot == 0U && next_generation == 0U,
        "clear ordinary-exclusive outputs after invalid active mode"
    );
}

void test_generation_saturation_does_not_allocate() {
    auto registry = initialized_registry();
    const std::uint64_t saturated = std::numeric_limits<std::uint64_t>::max();
    std::memcpy(registry.data() + k_header_generation_offset, &saturated, sizeof(saturated));

    std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t generation = std::numeric_limits<std::uint64_t>::max();
    check(
        allocate(
            registry,
            current_identity(),
            MYLITE_OWNERLESS_PROCESS_OPEN_MODE_OWNERLESS_RW,
            &slot,
            &generation
        ) == MYLITE_OWNERLESS_PROCESS_REGISTRY_ERROR,
        "reject generation saturation"
    );
    check(slot == 0U && generation == 0U, "clear outputs after generation saturation");
    check(
        mylite_ownerless_process_registry_active_count(registry.data()) == 0U,
        "do not allocate at generation saturation"
    );
}

} // namespace

int main() {
    test_ordinary_exclusive_blocks_every_active_mode();
    test_ownerless_modes_are_mutually_compatible();
    test_incompatible_allocations_are_serialized();
    test_active_stale_slot_remains_authoritative_until_cleanup();
    test_invalid_modes_fail_closed();
    test_generation_saturation_does_not_allocate();
    return 0;
}
