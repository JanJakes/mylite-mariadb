#include "mylite_ownerless_dictionary_hooks.h"

#include <atomic>

namespace
{

std::atomic<mylite_ownerless_dictionary_native_file_op_callback>
    native_file_op_callback{nullptr};
std::atomic<void *> callback_context{nullptr};

} // namespace

extern "C" void mylite_ownerless_dictionary_set_hooks(
    mylite_ownerless_dictionary_native_file_op_callback native_file_op_hook,
    void *context)
{
  if (native_file_op_hook == nullptr)
  {
    mylite_ownerless_dictionary_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  native_file_op_callback.store(native_file_op_hook,
                                std::memory_order_release);
}

extern "C" void mylite_ownerless_dictionary_reset_hooks(void)
{
  native_file_op_callback.store(nullptr, std::memory_order_release);
  callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_dictionary_has_hooks(void)
{
  return native_file_op_callback.load(std::memory_order_acquire) != nullptr;
}

extern "C" void mylite_ownerless_dictionary_native_file_op(void)
{
  mylite_ownerless_dictionary_native_file_op_callback hook=
      native_file_op_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return;

  hook(callback_context.load(std::memory_order_acquire));
}
