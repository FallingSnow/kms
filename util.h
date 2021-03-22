#pragma once

#include <time.h>

#define DEBUG(...) getenv("DEBUG") != NULL &&printf(__VA_ARGS__)
struct perf_clock {
  clock_t clock;
  struct timespec time;
};

inline struct perf_clock get_perf();
inline void timespec_diff(struct timespec *a, struct timespec *b,
                                 struct timespec *result);
void display_diff_perf(struct perf_clock *start, struct perf_clock *end);
int xioctl(int fh, int request, void *arg);
