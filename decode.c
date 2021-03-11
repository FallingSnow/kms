#include <bits/stdint-uintn.h>
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "drm.h"
#include "v4l2.h"

#define DRI_CARD_PATH "/dev/dri/card1"

struct perf_clock {
  clock_t clock;
  struct timespec time;
};

struct perf_clock get_perf() {
  struct timespec current_time = {0};
  clock_gettime(0, &current_time);
  struct perf_clock perf = {
      .clock = clock(),
      .time = current_time,
  };
  return perf;
}

void display_diff_perf(struct perf_clock *start, struct perf_clock *end) {
  // struct timespec diff = timespec_sub(end->time, start->time);

  double datetime_diff_ms = 0;
  double runtime_diff_ms = (end->clock - start->clock) * 1000. / CLOCKS_PER_SEC;

  printf("[Lower is Better] Time: %fms\tPerformance: %fms\n", datetime_diff_ms,
         runtime_diff_ms);
}

int main() {
  int err = 0, dri_fd = 0;
  struct Screen screens[10] = {0};
  struct Decoder decoder = {0};
  struct perf_clock start, end;

  init_decoder(&decoder);
  start_capturing(&decoder);
  // get_buffers(&decoder);
  // printf("First buffer address: %p\n", decoder.buffers[0].ptr);

  /*
   * Start to work with DRM/KMS
   */

  //------------------------------------------------------------------------------
  // Opening the DRI device
  //------------------------------------------------------------------------------

  dri_fd = open(DRI_CARD_PATH, O_RDWR);
  if (dri_fd < 0) {
    fprintf(stderr, "Unable to open %s\n", DRI_CARD_PATH);
    return -1;
  }
  printf("Opened dri file descriptor %d\n", dri_fd);

  err = drm_kms(dri_fd, screens);
  if (err) {
    printf("Failed to initialize drm kms, status = %d\n", err);
  }

  for (int i = 0; i < 1; i++) {
    printf("Connector ID: %d\n", screens[i].connector.connector_id);
    drm_set_mode(dri_fd, &screens[i]);
  }

  //------------------------------------------------------------------------------
  // Framebuffer rendering
  //------------------------------------------------------------------------------

  int x, y, page, num_screens = 1;
  struct Framebuffer *fb;
  void *tmp;

  start = get_perf();
  for (int i = 0; i < 60; i++) {
    int j;
    for (j = 0; j < num_screens; j++) {
      // Choose back buffer (Not the current buffer)
      page = screens[j].page == 0 ? 1 : 0;
      // printf("Current page: %d\t Target page: %d\n", screen[j].page, page);
      fb = &screens[j].buffers[page];

      long height = fb->height;
      long width = fb->width;

      int col = (rand() % 0x00ffffff) & 0x00ff00ff;

      for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
          int location = y * (width) + x;
          *(((uint32_t *)fb->ptr) + location) = col;
        }

      drm_swap_buffers_page_flip(dri_fd, fb, &screens[j].crtc);
      screens[j].page = page;
      // printf("Set new page: %d\n", screen[j].page);
    }
    // usleep(16666);
  }
  end = get_perf();

  display_diff_perf(&start, &end);

  /*
   * Clean up resources
   */
cleanup:

  // Stop being the "master" of the DRI device
  err = ioctl(dri_fd, DRM_IOCTL_DROP_MASTER, 0);
  if (err) {
    printf("Unable to drop KMS master, status = %d, status = %s\n", err,
           strerror(errno));
    return true;
  }

  for (int i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
    for (int b = 0; b < NUM_BUFFERS; b++) {
      struct Framebuffer buffer = screens[i].buffers[b];
      if (buffer.ptr != NULL)
        munmap(buffer.ptr, buffer.size);
    }
  }
  if (dri_fd > 0) {
    close(dri_fd);
  }
  return 0;
}
