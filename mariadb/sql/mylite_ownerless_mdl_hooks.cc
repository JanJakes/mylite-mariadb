#include "mylite_ownerless_mdl_hooks.h"

#include <atomic>

namespace {

std::atomic<mylite_ownerless_mdl_acquire_callback> acquire_callback{nullptr};
std::atomic<mylite_ownerless_mdl_release_callback> release_callback{nullptr};
std::atomic<void *> callback_context{nullptr};

} // namespace

extern "C" void mylite_ownerless_mdl_set_hooks(
    mylite_ownerless_mdl_acquire_callback acquire_hook,
    mylite_ownerless_mdl_release_callback release_hook,
    void *context)
{
  if (acquire_hook == nullptr || release_hook == nullptr)
  {
    mylite_ownerless_mdl_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  release_callback.store(release_hook, std::memory_order_release);
  acquire_callback.store(acquire_hook, std::memory_order_release);
}

extern "C" void mylite_ownerless_mdl_reset_hooks(void)
{
  acquire_callback.store(nullptr, std::memory_order_release);
  release_callback.store(nullptr, std::memory_order_release);
  callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_mdl_has_hooks(void)
{
  return acquire_callback.load(std::memory_order_acquire) != nullptr &&
         release_callback.load(std::memory_order_acquire) != nullptr;
}

extern "C" int mylite_ownerless_mdl_acquire(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout)
{
  mylite_ownerless_mdl_acquire_callback hook=
    acquire_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return MYLITE_OWNERLESS_MDL_OK;

  return hook(key, lock_wait_timeout,
              callback_context.load(std::memory_order_acquire));
}

extern "C" void mylite_ownerless_mdl_release(
    const mylite_ownerless_mdl_key_view *key)
{
  mylite_ownerless_mdl_release_callback hook=
    release_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return;

  hook(key, callback_context.load(std::memory_order_acquire));
}
