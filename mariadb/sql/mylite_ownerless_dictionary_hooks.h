#ifndef MYLITE_OWNERLESS_DICTIONARY_HOOKS_INCLUDED
#define MYLITE_OWNERLESS_DICTIONARY_HOOKS_INCLUDED

#ifdef __cplusplus
extern "C"
{
#endif

  typedef void (*mylite_ownerless_dictionary_native_file_op_callback)(
      void *context);

  void mylite_ownerless_dictionary_set_hooks(
      mylite_ownerless_dictionary_native_file_op_callback native_file_op_hook,
      void *context);
  void mylite_ownerless_dictionary_reset_hooks(void);
  int mylite_ownerless_dictionary_has_hooks(void);
  void mylite_ownerless_dictionary_native_file_op(void);

#ifdef __cplusplus
}
#endif

#endif
