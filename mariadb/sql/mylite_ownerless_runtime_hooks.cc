#include "mylite_ownerless_runtime_hooks.h"

#include <atomic>

namespace {

std::atomic<mylite_ownerless_runtime_shared_file_callback> delete_shared_file_callback{nullptr};
std::atomic<void *> callback_context{nullptr};

} // namespace

extern "C" void mylite_ownerless_runtime_set_hooks(
    mylite_ownerless_runtime_shared_file_callback delete_shared_file_hook,
    void *context)
{
  if (delete_shared_file_hook == nullptr)
  {
    mylite_ownerless_runtime_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  delete_shared_file_callback.store(delete_shared_file_hook, std::memory_order_release);
}

extern "C" void mylite_ownerless_runtime_reset_hooks(void)
{
  delete_shared_file_callback.store(nullptr, std::memory_order_release);
  callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_runtime_has_hooks(void)
{
  return delete_shared_file_callback.load(std::memory_order_acquire) != nullptr;
}

extern "C" int mylite_ownerless_runtime_may_delete_shared_file(void)
{
  mylite_ownerless_runtime_shared_file_callback hook =
      delete_shared_file_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return 1;

  return hook(callback_context.load(std::memory_order_acquire));
}
