#include <bits/stdint-uintn.h>
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm.h"
#include "util.h"
#include "v4l2.h"

#define DRI_CARD_PATH "/dev/dri/card0"

int main() {
  int err = 0, dri_fd = 0;
  FILE *in_fp;
  struct Screen screens[10] = {0};
  struct Decoder decoder = {0};
  struct perf_clock start, end;

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
    fprintf(stderr, "Failed to initialize drm kms, status = %d\n", err);
    return -1;
  }

  for (int i = 0; i < 1; i++) {
    printf("Connector ID: %d\n", screens[i].connector.connector_id);
    if (drm_set_mode(dri_fd, &screens[i]) < 0) {
      fprintf(stderr, "drm_set_mode failed, status = %d", err);
      return -1;
    }
  }

  //------------------------------------------------------------------------------
  // Framebuffer rendering
  //------------------------------------------------------------------------------

  // in_fp = fopen("/home/alarm/FPS_test_1080p60_L4.2.h264", "rb");
  in_fp = fopen("/home/alarm/jellyfish-15-mbps-hd-h264.h264", "rb");
  if (in_fp == NULL) {
    fprintf(stderr, "Failed to open input video");
    return -1;
  }

  decoder.output = screens[0].buffers[0].ptr;
  decoder.source = in_fp;
  init_decoder(&decoder);
  start_capturing(&decoder);
  // mainloop(&decoder);
  // run(&decoder);
  // get_buffers(&decoder);
  // printf("First buffer address: %p\n", decoder.buffers[0].ptr);
  int action;
  for (;;) {
    // start = get_perf();
    action = run(&decoder);
    // printf("Action %d\n", action);
    if (action == 1) {
      // Choose back buffer (Not the current buffer)
      int page = screens[0].page == 0 ? 1 : 0;
      // printf("Current page: %d\t Target page: %d\n", screen[j].page, page);
      struct Framebuffer *fb = &screens[0].buffers[page];
      // printf("Frame pointer: %lu\n", fb->size);

      // printf("Writing to FB: %d\n", fb->id);
      drm_swap_buffers_set_crtc(dri_fd, fb, &screens[0].crtc);
      screens[0].page = page;
      decoder.output = screens[0].buffers[page].ptr;
      // end = get_perf();
      // display_diff_perf(&start, &end);
    }
  }

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
