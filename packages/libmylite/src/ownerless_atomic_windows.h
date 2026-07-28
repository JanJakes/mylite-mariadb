#ifndef MYLITE_OWNERLESS_ATOMIC_WINDOWS_H
#define MYLITE_OWNERLESS_ATOMIC_WINDOWS_H

#if defined(_MSC_VER) && !defined(__clang__)

#  include <intrin.h>

#  include <cstdint>
#  include <type_traits>

#  ifndef __ATOMIC_RELAXED
#    define __ATOMIC_RELAXED 0
#    define __ATOMIC_ACQUIRE 2
#    define __ATOMIC_RELEASE 3
#    define __ATOMIC_ACQ_REL 4
#    define __ATOMIC_SEQ_CST 5
#  endif

template <typename T> T mylite_ownerless_atomic_load_n(const T *value, int order) {
    (void)order;
    static_assert(std::is_integral_v<T>, "ownerless atomics require integral words");
    static_assert(sizeof(T) == 4U || sizeof(T) == 8U, "unsupported ownerless atomic word");
    if constexpr (sizeof(T) == 4U) {
        const long observed = _InterlockedCompareExchange(
            reinterpret_cast<volatile long *>(const_cast<T *>(value)),
            0,
            0
        );
        return static_cast<T>(observed);
    } else {
        const long long observed = _InterlockedCompareExchange64(
            reinterpret_cast<volatile long long *>(const_cast<T *>(value)),
            0,
            0
        );
        return static_cast<T>(observed);
    }
}

template <typename T> void mylite_ownerless_atomic_store_n(T *target, T value, int order) {
    (void)order;
    static_assert(std::is_integral_v<T>, "ownerless atomics require integral words");
    static_assert(sizeof(T) == 4U || sizeof(T) == 8U, "unsupported ownerless atomic word");
    if constexpr (sizeof(T) == 4U) {
        static_cast<void>(_InterlockedExchange(
            reinterpret_cast<volatile long *>(target),
            static_cast<long>(value)
        ));
    } else {
        static_cast<void>(_InterlockedExchange64(
            reinterpret_cast<volatile long long *>(target),
            static_cast<long long>(value)
        ));
    }
}

template <typename T, typename U>
T mylite_ownerless_atomic_add_fetch(T *target, U value, int order) {
    (void)order;
    static_assert(std::is_integral_v<T>, "ownerless atomics require integral words");
    static_assert(std::is_integral_v<U>, "ownerless atomic increments require integral values");
    static_assert(sizeof(T) == 4U || sizeof(T) == 8U, "unsupported ownerless atomic word");
    const T converted_value = static_cast<T>(value);
    if constexpr (sizeof(T) == 4U) {
        return static_cast<T>(
            _InterlockedExchangeAdd(
                reinterpret_cast<volatile long *>(target),
                static_cast<long>(converted_value)
            ) +
            static_cast<long>(converted_value)
        );
    } else {
        return static_cast<T>(
            _InterlockedExchangeAdd64(
                reinterpret_cast<volatile long long *>(target),
                static_cast<long long>(converted_value)
            ) +
            static_cast<long long>(converted_value)
        );
    }
}

template <typename T, typename U>
T mylite_ownerless_atomic_sub_fetch(T *target, U value, int order) {
    const T converted_value = static_cast<T>(value);
    return mylite_ownerless_atomic_add_fetch(target, static_cast<T>(T{0} - converted_value), order);
}

template <typename T>
bool mylite_ownerless_atomic_compare_exchange_n(
    T *target,
    T *expected,
    T desired,
    bool weak,
    int success_order,
    int failure_order
) {
    (void)weak;
    (void)success_order;
    (void)failure_order;
    static_assert(std::is_integral_v<T>, "ownerless atomics require integral words");
    static_assert(sizeof(T) == 4U || sizeof(T) == 8U, "unsupported ownerless atomic word");
    T observed = {};
    if constexpr (sizeof(T) == 4U) {
        observed = static_cast<T>(_InterlockedCompareExchange(
            reinterpret_cast<volatile long *>(target),
            static_cast<long>(desired),
            static_cast<long>(*expected)
        ));
    } else {
        observed = static_cast<T>(_InterlockedCompareExchange64(
            reinterpret_cast<volatile long long *>(target),
            static_cast<long long>(desired),
            static_cast<long long>(*expected)
        ));
    }
    if (observed == *expected) {
        return true;
    }
    *expected = observed;
    return false;
}

#  define __atomic_load_n mylite_ownerless_atomic_load_n
#  define __atomic_store_n mylite_ownerless_atomic_store_n
#  define __atomic_add_fetch mylite_ownerless_atomic_add_fetch
#  define __atomic_sub_fetch mylite_ownerless_atomic_sub_fetch
#  define __atomic_compare_exchange_n mylite_ownerless_atomic_compare_exchange_n

#endif

#endif
