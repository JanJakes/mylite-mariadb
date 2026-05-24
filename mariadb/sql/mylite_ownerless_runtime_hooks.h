#ifndef MYLITE_OWNERLESS_RUNTIME_HOOKS_INCLUDED
#define MYLITE_OWNERLESS_RUNTIME_HOOKS_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*mylite_ownerless_runtime_shared_file_callback)(void *context);

void mylite_ownerless_runtime_set_hooks(
    mylite_ownerless_runtime_shared_file_callback delete_shared_file_hook,
    void *context);
void mylite_ownerless_runtime_reset_hooks(void);
int mylite_ownerless_runtime_has_hooks(void);
int mylite_ownerless_runtime_may_delete_shared_file(void);

#ifdef __cplusplus
}
#endif

#endif
