#include "mylite_ownerless_trx_hooks.h"

#include "trx0sys.h"

#include <atomic>

std::atomic<bool> mylite_ownerless_trx_hooks_enabled{false};

namespace {

std::atomic<mylite_ownerless_trx_allocate_callback> allocate_callback{nullptr};
std::atomic<mylite_ownerless_trx_register_callback> register_callback{nullptr};
std::atomic<mylite_ownerless_trx_assign_no_callback> assign_no_callback{nullptr};
std::atomic<mylite_ownerless_trx_deregister_callback> deregister_callback{nullptr};
std::atomic<mylite_ownerless_trx_snapshot_callback> snapshot_callback{nullptr};
std::atomic<mylite_ownerless_trx_next_id_callback> next_id_callback{nullptr};
std::atomic<mylite_ownerless_trx_rollback_state_callback>
    rollback_state_callback{nullptr};
std::atomic<mylite_ownerless_trx_recovery_state_callback>
    recovery_state_callback{nullptr};
std::atomic<void *> callback_context{nullptr};

int normalize_result(bool hooks_required, int result)
{
  return hooks_required && result == MYLITE_OWNERLESS_TRX_UNAVAILABLE
    ? MYLITE_OWNERLESS_TRX_ERROR
    : result;
}

} // namespace

extern "C" void mylite_ownerless_trx_set_hooks(
    mylite_ownerless_trx_allocate_callback allocate_hook,
    mylite_ownerless_trx_register_callback register_hook,
    mylite_ownerless_trx_assign_no_callback assign_no_hook,
    mylite_ownerless_trx_deregister_callback deregister_hook,
    mylite_ownerless_trx_snapshot_callback snapshot_hook,
    mylite_ownerless_trx_next_id_callback next_id_hook,
    mylite_ownerless_trx_rollback_state_callback rollback_state_hook,
    mylite_ownerless_trx_recovery_state_callback recovery_state_hook,
    void *context)
{
  if (allocate_hook == nullptr || register_hook == nullptr ||
      assign_no_hook == nullptr || deregister_hook == nullptr ||
      snapshot_hook == nullptr || next_id_hook == nullptr ||
      rollback_state_hook == nullptr ||
      recovery_state_hook == nullptr)
  {
    mylite_ownerless_trx_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  next_id_callback.store(next_id_hook, std::memory_order_release);
  rollback_state_callback.store(rollback_state_hook, std::memory_order_release);
  recovery_state_callback.store(recovery_state_hook, std::memory_order_release);
  snapshot_callback.store(snapshot_hook, std::memory_order_release);
  deregister_callback.store(deregister_hook, std::memory_order_release);
  assign_no_callback.store(assign_no_hook, std::memory_order_release);
  register_callback.store(register_hook, std::memory_order_release);
  allocate_callback.store(allocate_hook, std::memory_order_release);
  mylite_ownerless_trx_hooks_enabled.store(true, std::memory_order_release);
}

extern "C" void mylite_ownerless_trx_reset_hooks(void)
{
  mylite_ownerless_trx_hooks_enabled.store(false, std::memory_order_release);
  allocate_callback.store(nullptr, std::memory_order_release);
  next_id_callback.store(nullptr, std::memory_order_release);
  rollback_state_callback.store(nullptr, std::memory_order_release);
  recovery_state_callback.store(nullptr, std::memory_order_release);
  register_callback.store(nullptr, std::memory_order_release);
  assign_no_callback.store(nullptr, std::memory_order_release);
  deregister_callback.store(nullptr, std::memory_order_release);
  snapshot_callback.store(nullptr, std::memory_order_release);
  callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_trx_has_hooks(void)
{
  if (!mylite_ownerless_trx_hooks_enabled_fast())
    return 0;

  return allocate_callback.load(std::memory_order_acquire) != nullptr &&
         register_callback.load(std::memory_order_acquire) != nullptr &&
         assign_no_callback.load(std::memory_order_acquire) != nullptr &&
         deregister_callback.load(std::memory_order_acquire) != nullptr &&
         snapshot_callback.load(std::memory_order_acquire) != nullptr &&
         next_id_callback.load(std::memory_order_acquire) != nullptr &&
         rollback_state_callback.load(std::memory_order_acquire) != nullptr &&
         recovery_state_callback.load(std::memory_order_acquire) != nullptr;
}

extern "C" uint64_t mylite_ownerless_trx_local_max_id(void)
{
  return trx_sys.get_local_max_trx_id();
}

extern "C" void
mylite_ownerless_trx_advance_local_max_id_at_least(uint64_t minimum_next_trx_id)
{
  trx_sys.advance_max_trx_id_at_least(
      static_cast<trx_id_t>(minimum_next_trx_id));
}

extern "C" int mylite_ownerless_trx_allocate(uint64_t *out_trx_id)
{
  const bool hooks_required= mylite_ownerless_trx_hooks_enabled_fast();
  mylite_ownerless_trx_allocate_callback hook=
    allocate_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return hooks_required
      ? MYLITE_OWNERLESS_TRX_ERROR
      : MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return normalize_result(
    hooks_required,
    hook(out_trx_id, callback_context.load(std::memory_order_acquire)));
}

extern "C" int mylite_ownerless_trx_register(uint64_t *out_trx_id)
{
  const bool hooks_required= mylite_ownerless_trx_hooks_enabled_fast();
  mylite_ownerless_trx_register_callback hook=
    register_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return hooks_required
      ? MYLITE_OWNERLESS_TRX_ERROR
      : MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return normalize_result(
    hooks_required,
    hook(out_trx_id, callback_context.load(std::memory_order_acquire)));
}

extern "C" int mylite_ownerless_trx_assign_no(uint64_t trx_id, uint64_t *out_trx_no)
{
  const bool hooks_required= mylite_ownerless_trx_hooks_enabled_fast();
  mylite_ownerless_trx_assign_no_callback hook=
    assign_no_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return hooks_required
      ? MYLITE_OWNERLESS_TRX_ERROR
      : MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return normalize_result(
    hooks_required,
    hook(trx_id, out_trx_no,
         callback_context.load(std::memory_order_acquire)));
}

extern "C" int mylite_ownerless_trx_deregister(uint64_t trx_id)
{
  const bool hooks_required= mylite_ownerless_trx_hooks_enabled_fast();
  mylite_ownerless_trx_deregister_callback hook=
    deregister_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return hooks_required
      ? MYLITE_OWNERLESS_TRX_ERROR
      : MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  /* Deregistration callers retain their token and distinguish a strict first
  UNAVAILABLE response from an idempotent retry after an ambiguous result. */
  return hook(trx_id, callback_context.load(std::memory_order_acquire));
}

extern "C" int mylite_ownerless_trx_snapshot(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no)
{
  const bool hooks_required= mylite_ownerless_trx_hooks_enabled_fast();
  mylite_ownerless_trx_snapshot_callback hook=
    snapshot_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return hooks_required
      ? MYLITE_OWNERLESS_TRX_ERROR
      : MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return normalize_result(
    hooks_required,
    hook(out_trx_ids, trx_id_capacity, out_trx_id_count,
         out_next_trx_id, out_min_trx_no,
         callback_context.load(std::memory_order_acquire)));
}

extern "C" int mylite_ownerless_trx_snapshot_retry(
    uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no)
{
  static constexpr unsigned int retry_count= 3;
  int result= MYLITE_OWNERLESS_TRX_ERROR;

  for (unsigned int attempt= 0; attempt < retry_count; ++attempt)
  {
    result= mylite_ownerless_trx_snapshot(
        out_trx_ids, trx_id_capacity, out_trx_id_count, out_next_trx_id,
        out_min_trx_no);
    if (result != MYLITE_OWNERLESS_TRX_ERROR)
      return result;

    ut_delay(1000);
  }

  return result;
}

extern "C" uint64_t mylite_ownerless_trx_next_id(void)
{
  mylite_ownerless_trx_next_id_callback hook=
    next_id_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return 0;

  return hook(callback_context.load(std::memory_order_acquire));
}

extern "C" int mylite_ownerless_trx_set_rollback_state(
    uint64_t trx_id, uint32_t rollback_state)
{
  if (trx_id == 0 || rollback_state >
      MYLITE_OWNERLESS_TRX_ROLLBACK_SAVEPOINT_READ_SAFE)
    return MYLITE_OWNERLESS_TRX_ERROR;

  const bool hooks_required= mylite_ownerless_trx_hooks_enabled_fast();
  mylite_ownerless_trx_rollback_state_callback hook=
      rollback_state_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return hooks_required
      ? MYLITE_OWNERLESS_TRX_ERROR
      : MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  return normalize_result(
      hooks_required,
      hook(trx_id, rollback_state,
           callback_context.load(std::memory_order_acquire)));
}

extern "C" int mylite_ownerless_trx_recovery_state(
    uint64_t trx_id, int *out_state)
{
  if (out_state == nullptr || trx_id == 0)
    return MYLITE_OWNERLESS_TRX_ERROR;
  *out_state= MYLITE_OWNERLESS_TRX_RECOVERY_BLOCKED;

  const bool hooks_required= mylite_ownerless_trx_hooks_enabled_fast();
  mylite_ownerless_trx_recovery_state_callback hook=
      recovery_state_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return hooks_required
      ? MYLITE_OWNERLESS_TRX_ERROR
      : MYLITE_OWNERLESS_TRX_UNAVAILABLE;

  const int result= normalize_result(
      hooks_required,
      hook(trx_id, out_state,
           callback_context.load(std::memory_order_acquire)));
  if (result != MYLITE_OWNERLESS_TRX_OK ||
      (*out_state != MYLITE_OWNERLESS_TRX_RECOVERY_ABSENT &&
       *out_state != MYLITE_OWNERLESS_TRX_RECOVERY_LIVE_REMOTE &&
       *out_state != MYLITE_OWNERLESS_TRX_RECOVERY_REQUIRED &&
       *out_state != MYLITE_OWNERLESS_TRX_RECOVERY_BLOCKED))
  {
    *out_state= MYLITE_OWNERLESS_TRX_RECOVERY_BLOCKED;
    return MYLITE_OWNERLESS_TRX_ERROR;
  }
  return MYLITE_OWNERLESS_TRX_OK;
}
