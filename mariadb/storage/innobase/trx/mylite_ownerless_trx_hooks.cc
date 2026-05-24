#include "mylite_ownerless_trx_hooks.h"

#include "trx0sys.h"

#include <atomic>

namespace {

std::atomic<mylite_ownerless_trx_allocate_callback> allocate_callback{nullptr};
std::atomic<mylite_ownerless_trx_register_callback> register_callback{nullptr};
std::atomic<mylite_ownerless_trx_assign_no_callback> assign_no_callback{nullptr};
std::atomic<mylite_ownerless_trx_deregister_callback> deregister_callback{nullptr};
std::atomic<mylite_ownerless_trx_snapshot_callback> snapshot_callback{nullptr};
std::atomic<void *> callback_context{nullptr};

} // namespace

extern "C" void mylite_ownerless_trx_set_hooks(
    mylite_ownerless_trx_allocate_callback allocate_hook,
    mylite_ownerless_trx_register_callback register_hook,
    mylite_ownerless_trx_assign_no_callback assign_no_hook,
    mylite_ownerless_trx_deregister_callback deregister_hook,
    mylite_ownerless_trx_snapshot_callback snapshot_hook,
    void *context)
{
  if (allocate_hook == nullptr || register_hook == nullptr ||
      assign_no_hook == nullptr || deregister_hook == nullptr ||
      snapshot_hook == nullptr)
  {
    mylite_ownerless_trx_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  snapshot_callback.store(snapshot_hook, std::memory_order_release);
  deregister_callback.store(deregister_hook, std::memory_order_release);
  assign_no_callback.store(assign_no_hook, std::memory_order_release);
  register_callback.store(register_hook, std::memory_order_release);
  allocate_callback.store(allocate_hook, std::memory_order_release);
}

extern "C" void mylite_ownerless_trx_reset_hooks(void)
{
  allocate_callback.store(nullptr, std::memory_order_release);
  register_callback.store(nullptr, std::memory_order_release);
  assign_no_callback.store(nullptr, std::memory_order_release);
  deregister_callback.store(nullptr, std::memory_order_release);
  snapshot_callback.store(nullptr, std::memory_order_release);
  callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_trx_has_hooks(void)
{
  return allocate_callback.load(std::memory_order_acquire) != nullptr &&
         register_callback.load(std::memory_order_acquire) != nullptr &&
         assign_no_callback.load(std::memory_order_acquire) != nullptr &&
         deregister_callback.load(std::memory_order_acquire) != nullptr &&
         snapshot_callback.load(std::memory_order_acquire) != nullptr;
}

extern "C" uint64_t mylite_ownerless_trx_local_max_id(void)
{
  return trx_sys.get_local_max_trx_id();
}

extern "C" int mylite_ownerless_trx_allocate(uint64_t *out_trx_id)
{
  mylite_ownerless_trx_allocate_callback hook=
    allocate_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return hook(out_trx_id, callback_context.load(std::memory_order_acquire));
}

extern "C" int mylite_ownerless_trx_register(uint64_t *out_trx_id)
{
  mylite_ownerless_trx_register_callback hook=
    register_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return hook(out_trx_id, callback_context.load(std::memory_order_acquire));
}

extern "C" int mylite_ownerless_trx_assign_no(uint64_t trx_id, uint64_t *out_trx_no)
{
  mylite_ownerless_trx_assign_no_callback hook=
    assign_no_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return hook(trx_id, out_trx_no, callback_context.load(std::memory_order_acquire));
}

extern "C" int mylite_ownerless_trx_deregister(uint64_t trx_id)
{
  mylite_ownerless_trx_deregister_callback hook=
    deregister_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return hook(trx_id, callback_context.load(std::memory_order_acquire));
}

extern "C" int mylite_ownerless_trx_snapshot(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no)
{
  mylite_ownerless_trx_snapshot_callback hook=
    snapshot_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return hook(out_trx_ids, trx_id_capacity, out_trx_id_count,
              out_next_trx_id, out_min_trx_no,
              callback_context.load(std::memory_order_acquire));
}
