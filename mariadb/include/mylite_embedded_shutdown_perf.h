#ifndef MYLITE_EMBEDDED_SHUTDOWN_PERF_H
#define MYLITE_EMBEDDED_SHUTDOWN_PERF_H

#include <stddef.h>
#include <stdint.h>

enum mylite_embedded_shutdown_perf_stat_index {
  MYLITE_EMBEDDED_SHUTDOWN_PERF_SERVER_END_CALLS= 0,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_SERVER_END_TOTAL_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_SERVER_END_CLIENT_PLUGIN_DEINIT_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_SERVER_END_FINISH_CLIENT_ERRS_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_SERVER_END_VIO_END_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_SERVER_END_EMBEDDED_SERVER_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_SERVER_END_MY_END_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_END_EMBEDDED_SERVER_CALLS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_END_EMBEDDED_SERVER_TOTAL_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_END_EMBEDDED_SERVER_FREE_ARGS_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_END_EMBEDDED_SERVER_CLEAN_UP_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_END_EMBEDDED_SERVER_CLEAN_UP_MUTEXES_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_CALLS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_TOTAL_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_EARLY_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_PLUGIN_SHUTDOWN_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_HANDLER_END_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_TDC_MDL_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_CACHE_STATUS_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_SCHEDULER_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_MYSQL_LIBRARY_END_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_ERROR_CHARSET_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_CLEAN_UP_FINAL_FREE_NS,
  MYLITE_EMBEDDED_SHUTDOWN_PERF_STAT_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif

void mylite_embedded_shutdown_perf_set_enabled(int enabled);
void mylite_embedded_shutdown_perf_reset(void);
void mylite_embedded_shutdown_perf_read(uint64_t *out_values, size_t value_count);
int mylite_embedded_shutdown_perf_stats_enabled(void);
uint64_t mylite_embedded_shutdown_perf_now_ns(void);
void mylite_embedded_shutdown_perf_add(size_t index, uint64_t value);
void mylite_embedded_shutdown_perf_add_elapsed(size_t index, uint64_t start_ns);

#ifdef __cplusplus
}
#endif

static inline uint64_t mylite_embedded_shutdown_perf_start_ns(void)
{
  return mylite_embedded_shutdown_perf_stats_enabled()
             ? mylite_embedded_shutdown_perf_now_ns()
             : 0;
}

static inline void mylite_embedded_shutdown_perf_count(size_t index)
{
  if (mylite_embedded_shutdown_perf_stats_enabled())
    mylite_embedded_shutdown_perf_add(index, 1);
}

#endif
