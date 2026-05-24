#include "mylite_ownerless_read_view_hooks.h"

#include <atomic>

namespace {

std::atomic<mylite_ownerless_read_view_register_callback> register_callback{nullptr};
std::atomic<mylite_ownerless_read_view_deregister_callback> deregister_callback{nullptr};
std::atomic<mylite_ownerless_read_view_snapshot_callback> snapshot_callback{nullptr};
std::atomic<void *> callback_context{nullptr};

} // namespace

extern "C" void mylite_ownerless_read_view_set_hooks(
    mylite_ownerless_read_view_register_callback register_hook,
    mylite_ownerless_read_view_deregister_callback deregister_hook,
    mylite_ownerless_read_view_snapshot_callback snapshot_hook,
    void *context)
{
  if (register_hook == nullptr || deregister_hook == nullptr ||
      snapshot_hook == nullptr || context == nullptr)
  {
    mylite_ownerless_read_view_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  register_callback.store(register_hook, std::memory_order_release);
  deregister_callback.store(deregister_hook, std::memory_order_release);
  snapshot_callback.store(snapshot_hook, std::memory_order_release);
}

extern "C" void mylite_ownerless_read_view_reset_hooks(void)
{
  snapshot_callback.store(nullptr, std::memory_order_release);
  deregister_callback.store(nullptr, std::memory_order_release);
  register_callback.store(nullptr, std::memory_order_release);
  callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_read_view_has_hooks(void)
{
  return register_callback.load(std::memory_order_acquire) != nullptr &&
         deregister_callback.load(std::memory_order_acquire) != nullptr &&
         snapshot_callback.load(std::memory_order_acquire) != nullptr;
}

extern "C" int mylite_ownerless_read_view_register(
    uint64_t low_limit_id,
    uint64_t low_limit_no,
    const uint64_t *trx_ids,
    unsigned int trx_id_count,
    uint32_t *out_slot_index,
    uint64_t *out_slot_generation)
{
  mylite_ownerless_read_view_register_callback hook=
      register_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  return hook == nullptr || context == nullptr
             ? MYLITE_OWNERLESS_READ_VIEW_UNAVAILABLE
             : hook(low_limit_id, low_limit_no, trx_ids, trx_id_count,
                    out_slot_index, out_slot_generation, context);
}

extern "C" int mylite_ownerless_read_view_deregister(
    uint32_t slot_index,
    uint64_t slot_generation)
{
  mylite_ownerless_read_view_deregister_callback hook=
      deregister_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  return hook == nullptr || context == nullptr
             ? MYLITE_OWNERLESS_READ_VIEW_UNAVAILABLE
             : hook(slot_index, slot_generation, context);
}

extern "C" int mylite_ownerless_read_view_snapshot(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_low_limit_id,
    uint64_t *out_low_limit_no)
{
  mylite_ownerless_read_view_snapshot_callback hook=
      snapshot_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  return hook == nullptr || context == nullptr
             ? MYLITE_OWNERLESS_READ_VIEW_UNAVAILABLE
             : hook(out_trx_ids, trx_id_capacity, out_trx_id_count,
                    out_low_limit_id, out_low_limit_no, context);
}
