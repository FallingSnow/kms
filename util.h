#pragma once

#include <time.h>

struct perf_clock {
  clock_t clock;
  struct timespec time;
};

struct perf_clock get_perf();
static inline void timespec_diff(struct timespec *a, struct timespec *b,
                                 struct timespec *result);
void display_diff_perf(struct perf_clock *start, struct perf_clock *end);
