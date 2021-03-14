#include <stdio.h>

#include "util.h"

struct perf_clock get_perf() {
  struct timespec current_time = {0};
  clock_gettime(0, &current_time);
  struct perf_clock perf = {
      .clock = clock(),
      .time = current_time,
  };
  return perf;
}

/**
 * @fn timespec_diff(struct timespec *, struct timespec *, struct timespec *)
 * @brief Compute the diff of two timespecs, that is a - b = result.
 * @param a the minuend
 * @param b the subtrahend
 * @param result a - b
 */
static inline void timespec_diff(struct timespec *a, struct timespec *b,
                                 struct timespec *result) {
  result->tv_sec = a->tv_sec - b->tv_sec;
  result->tv_nsec = a->tv_nsec - b->tv_nsec;
  if (result->tv_nsec < 0) {
    --result->tv_sec;
    result->tv_nsec += 1000000000L;
  }
}

void display_diff_perf(struct perf_clock *start, struct perf_clock *end) {
  struct timespec diff = {0};
  timespec_diff(&end->time, &start->time, &diff);

  double datetime_diff_ms = diff.tv_sec * 1000 + (float)diff.tv_nsec / 1000000;
  double runtime_diff_ms = (end->clock - start->clock) * 1000. / CLOCKS_PER_SEC;

  printf("[Lower is Better] Time: %fms\tPerformance: %fms\n", datetime_diff_ms,
         runtime_diff_ms);
}