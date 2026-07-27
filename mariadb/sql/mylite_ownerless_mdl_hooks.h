#ifndef MYLITE_OWNERLESS_MDL_HOOKS_INCLUDED
#define MYLITE_OWNERLESS_MDL_HOOKS_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
#include <atomic>

extern std::atomic<bool> mylite_ownerless_mdl_hooks_enabled;

static inline int mylite_ownerless_mdl_hooks_enabled_fast(void)
{
  return mylite_ownerless_mdl_hooks_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" {
#endif

#define MYLITE_OWNERLESS_MDL_OK 0
#define MYLITE_OWNERLESS_MDL_TIMEOUT 1
#define MYLITE_OWNERLESS_MDL_ERROR 2
#define MYLITE_OWNERLESS_MDL_DEADLOCK 3
#define MYLITE_OWNERLESS_MDL_KILLED 4
#define MYLITE_OWNERLESS_MDL_FULL 5

#define MYLITE_OWNERLESS_MDL_MODE_NONE 0U
#define MYLITE_OWNERLESS_MDL_MODE_SHARED 1U
#define MYLITE_OWNERLESS_MDL_MODE_EXCLUSIVE 2U
#define MYLITE_OWNERLESS_MDL_MODE_UPGRADABLE 3U
#define MYLITE_OWNERLESS_MDL_MODE_SHARED_READ 4U
#define MYLITE_OWNERLESS_MDL_MODE_SHARED_WRITE 5U
#define MYLITE_OWNERLESS_MDL_MODE_SHARED_READ_ONLY 6U
#define MYLITE_OWNERLESS_MDL_MODE_SHARED_NO_WRITE 7U
#define MYLITE_OWNERLESS_MDL_MODE_SHARED_NO_READ_WRITE 8U
#define MYLITE_OWNERLESS_MDL_MODE_SCOPED_INTENTION_EXCLUSIVE 9U

#ifndef MYLITE_OWNERLESS_MDL_WAIT_OPTIONS_DEFINED
#define MYLITE_OWNERLESS_MDL_WAIT_OPTIONS_DEFINED
typedef int (*mylite_ownerless_mdl_cancel_callback)(void *context);
typedef struct mylite_ownerless_mdl_wait_options {
  mylite_ownerless_mdl_cancel_callback is_cancelled;
  void *cancel_context;
  unsigned int bypass_queued_waiters;
  unsigned int deadlock_weight;
  unsigned int reclassify_from_mode;
  uint64_t max_write_lock_count;
} mylite_ownerless_mdl_wait_options;
#endif

typedef struct mylite_ownerless_mdl_key_view {
  unsigned int namespace_id;
  unsigned int lock_type;
  unsigned int lock_duration;
  unsigned int ownerless_mode;
  uint64_t session_id;
  mylite_ownerless_mdl_wait_options wait_options;
  const char *database_name;
  unsigned int database_name_length;
  const char *object_name;
  unsigned int object_name_length;
} mylite_ownerless_mdl_key_view;

typedef int (*mylite_ownerless_mdl_acquire_callback)(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout,
    void *context);
typedef void (*mylite_ownerless_mdl_release_callback)(
    const mylite_ownerless_mdl_key_view *key,
    void *context);

void mylite_ownerless_mdl_set_hooks(
    mylite_ownerless_mdl_acquire_callback acquire_hook,
    mylite_ownerless_mdl_release_callback release_hook,
    void *context);
void mylite_ownerless_mdl_reset_hooks(void);
int mylite_ownerless_mdl_has_hooks(void);
int mylite_ownerless_mdl_acquire(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout);
void mylite_ownerless_mdl_release(const mylite_ownerless_mdl_key_view *key);
uint64_t mylite_ownerless_mdl_test_allocate_session_id(void);
void mylite_ownerless_mdl_test_set_session_sequence(uint64_t next_session_id);

#ifdef __cplusplus
}
#endif

#endif
