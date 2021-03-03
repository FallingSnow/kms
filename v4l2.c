#include <bits/stdint-uintn.h>
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
#include <unistd.h>

#define DRI_CARD_PATH "/dev/dri/card0"
#define NUM_BUFFERS 2

struct Framebuffer {
  void *ptr;
  uint64_t size;
  long width;
  long height;
  int id;
};

struct Screen {
  struct Framebuffer buffers[NUM_BUFFERS];
  int page;
  struct drm_mode_crtc crtc;
};

bool swap_buffers(int dri_fd, struct Framebuffer *fb, struct drm_mode_crtc *crtc) {
  int err;
  crtc->fb_id = fb->id;

  err = ioctl(dri_fd, DRM_IOCTL_SET_MASTER, 0);
  if (err < 0) {
    printf("Error opening dri device, status = %d\n", err);
    return true;
  }

  err = ioctl(dri_fd, DRM_IOCTL_MODE_SETCRTC, crtc); // get crtc
  if (err) {
    printf("Unable to set crtc, status = %d\n", err);
    return true;
  }

  // Stop being the "master" of the DRI device
  err = ioctl(dri_fd, DRM_IOCTL_DROP_MASTER, 0);
  if (err) {
    printf("Unable to drop KMS master, status = %d\n", err);
    return true;
  }

  return false;
}

int main() {
  int hw_dec_fd = 0, err = 0, dri_fd = 0, num_screens = 0;
  struct v4l2_format format;
  uint64_t res_fb_buf[10] = {0}, res_crtc_buf[10] = {0}, res_conn_buf[10] = {0},
           res_enc_buf[10] = {0};
  struct drm_mode_card_res res = {0};
  struct drm_mode_modeinfo conn_mode_buf[50] = {0};
  uint64_t conn_prop_buf[50] = {0}, conn_propval_buf[50] = {0},
           conn_enc_buf[50] = {0};
  struct Screen screen[10] = {0};
  struct drm_mode_create_dumb create_dumb = {0};
  struct drm_mode_map_dumb map_dumb = {0};
  struct drm_mode_fb_cmd cmd_dumb = {0};
  struct drm_mode_get_connector conn = {0};
  struct drm_mode_get_encoder enc = {0};
  struct Framebuffer *buffer = NULL;

  hw_dec_fd = open("/dev/video19", 0);
  if (hw_dec_fd < 0) {
    printf("Error opening decode device, status = %d\n", hw_dec_fd);
    goto cleanup;
  }
  printf("Opened decode file descriptor %d\n", hw_dec_fd);

  // bzero(&format, sizeof(format));
  // err = ioctl(hw_dec_fd, VIDIOC_G_FMT, &format);
  // if (err != 0) {
  //   printf("Error getting formats, status = %d\n", err);
  // }

  /*
   * Start to work with DRM/KMS
   */

  //------------------------------------------------------------------------------
  // Opening the DRI device
  //------------------------------------------------------------------------------

  dri_fd = open(DRI_CARD_PATH, O_RDWR);

  //------------------------------------------------------------------------------
  // Kernel Mode Setting (KMS)
  //------------------------------------------------------------------------------

  err = ioctl(dri_fd, DRM_IOCTL_SET_MASTER, 0);
  if (err < 0) {
    printf("Error opening dri device, status = %d\n", err);
    goto cleanup;
  }
  printf("Opened dri file descriptor %d\n", dri_fd);

  // Get resource counts
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
  if (err < 0) {
    printf("Error getting mode resources, status = %d\n", err);
    goto cleanup;
  }
  res.fb_id_ptr = (uint64_t)res_fb_buf;
  res.crtc_id_ptr = (uint64_t)res_crtc_buf;
  res.connector_id_ptr = (uint64_t)res_conn_buf;
  res.encoder_id_ptr = (uint64_t)res_enc_buf;

  // Get resource IDs
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
  if (err < 0) {
    printf("Error getting mode resources ids, status = %d\n", err);
    goto cleanup;
  }

  printf("%s has %d connectors\n", DRI_CARD_PATH, res.count_connectors);

  int i;
  for (i = 0; i < res.count_connectors; i++) {

    conn.connector_id = res_conn_buf[i];

    printf("Getting connector %d\n", i);
    err = ioctl(dri_fd, DRM_IOCTL_MODE_GETCONNECTOR,
                &conn); // get connector resource counts
    if (err) {
      printf("Unable to get connector %d, status = %d\n", i, err);
      continue;
    }
    conn.modes_ptr = (uint64_t)conn_mode_buf;
    conn.props_ptr = (uint64_t)conn_prop_buf;
    conn.prop_values_ptr = (uint64_t)conn_propval_buf;
    conn.encoders_ptr = (uint64_t)conn_enc_buf;

    printf("[#%d] Getting connector resource ids\n", conn.connector_id);
    err = ioctl(dri_fd, DRM_IOCTL_MODE_GETCONNECTOR,
                &conn); // get connector resource IDs
    if (err) {
      printf("Failed to get connector ids, status = %d\n", err);
    }

    if (conn.count_encoders < 1 || conn.count_modes < 1 || !conn.encoder_id ||
        !conn.connection) {
      printf("[#%d] encoders: %d\tmodes: %d\n", conn.connector_id,
             conn.count_encoders, conn.count_modes);
      continue;
    }

    //------------------------------------------------------------------------------
    // Creating a dumb buffer
    //------------------------------------------------------------------------------

    // We create NUM_BUFFERS buffers for each screen. These are swapped out
    // during a VSync.
    for (int b = 0; b < NUM_BUFFERS; b++) {
      buffer = &screen[i].buffers[b];
      // If we create the buffer later, we can get the size of the screen first.
      // This must be a valid mode, so it's probably best to do this after we
      // find a valid crtc with modes.
      create_dumb.width = conn_mode_buf[0].hdisplay;
      create_dumb.height = conn_mode_buf[0].vdisplay;
      create_dumb.bpp = 32;
      create_dumb.flags = 0;
      create_dumb.pitch = 0;
      create_dumb.size = 0;
      create_dumb.handle = 0;
      err = ioctl(dri_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
      if (err) {
        printf("Failed to create dumb buffer, status = %d\n", err);
      }

      cmd_dumb.width = create_dumb.width;
      cmd_dumb.height = create_dumb.height;
      cmd_dumb.bpp = create_dumb.bpp;
      cmd_dumb.pitch = create_dumb.pitch;
      cmd_dumb.depth = 24;
      cmd_dumb.handle = create_dumb.handle;
      err = ioctl(dri_fd, DRM_IOCTL_MODE_ADDFB, &cmd_dumb);
      if (err) {
        printf("Failed to add frame buffer, status = %d\n", err);
      }

      map_dumb.handle = create_dumb.handle;
      err = ioctl(dri_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
      if (err) {
        printf("Failed to set dumb buffer to dumb mode, status = %d\n", err);
      }

      printf("[#%d] Created dumb buffer %d of %llu bytes\n", conn.connector_id,
             cmd_dumb.fb_id, create_dumb.size);

      buffer->ptr = mmap(0, create_dumb.size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, dri_fd, map_dumb.offset);
      buffer->size = create_dumb.size;
      buffer->width = create_dumb.width;
      buffer->height = create_dumb.height;
      buffer->id = cmd_dumb.fb_id;
    }

    //------------------------------------------------------------------------------
    // Kernel Mode Setting (KMS)
    //------------------------------------------------------------------------------

    printf("connection: %d -- mode: %d, prop: %d, enc: %d\n", conn.connection,
           conn.count_modes, conn.count_props, conn.count_encoders);
    printf("modes: %dx%d\n", conn_mode_buf[0].hdisplay,
           conn_mode_buf[0].vdisplay);

    enc.encoder_id = conn.encoder_id;
    err = ioctl(dri_fd, DRM_IOCTL_MODE_GETENCODER, &enc); // get encoder
    if (err) {
      printf("Unable to get encoder, status = %d\n", err);
    }

    struct drm_mode_crtc *crtc = &screen[i].crtc;
    crtc->crtc_id = enc.crtc_id;
    err = ioctl(dri_fd, DRM_IOCTL_MODE_GETCRTC, &crtc); // get crtc
    if (err) {
      printf("Unable to get crtc, status = %d\n", err);
    }

    crtc->fb_id = screen[i].buffers[0].id;
    crtc->set_connectors_ptr = (uint64_t)&res_conn_buf[i];
    crtc->count_connectors = 1;
    crtc->mode = conn_mode_buf[0];
    crtc->mode_valid = 1;
    err = ioctl(dri_fd, DRM_IOCTL_MODE_SETCRTC, crtc); // get crtc
    if (err) {
      printf("Unable to set crtc, status = %d\n", err);
    }

    printf("Set crtc mode\n");
    num_screens += 1;
  }

  // Stop being the "master" of the DRI device
  err = ioctl(dri_fd, DRM_IOCTL_DROP_MASTER, 0);
  if (err) {
    printf("Unable to drop KMS master, status = %d\n", err);
    return true;
  }

  printf("Finished creating DRM buffers\n");

  int x, y;
  void *tmp;
  for (i = 0; i < 60; i++) {
    int j;
    for (j = 0; j < num_screens; j++) {

      // Choose back buffer (Not the current buffer)
      int page = screen[j].page == 0 ? 1 : 0;
      // printf("Current page: %d\t Target page: %d\n", screen[j].page, page);
      struct Framebuffer *fb = &screen[j].buffers[page];

      long height = fb->height;
      long width = fb->width;

      int col = (rand() % 0x00ffffff) & 0x00ff00ff;

      for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
          int location = y * (width) + x;
          *(((uint32_t *)fb->ptr) + location) = col;
        }

      swap_buffers(dri_fd, fb, &screen[j].crtc);
      screen[j].page = page;
      // printf("Set new page: %d\n", screen[j].page);
    }
    // usleep(100000);
  }

  /*
   * Clean up resources
   */
cleanup:
  for (int i = 0; i < sizeof(screen) / sizeof(screen[0]); i++) {
    for (int b = 0; b < NUM_BUFFERS; b++) {
      struct Framebuffer buffer = screen[i].buffers[b];
      if (buffer.ptr != NULL)
        munmap(buffer.ptr, buffer.size);
    }
  }
  if (hw_dec_fd > 0) {
    close(hw_dec_fd);
  }
  if (dri_fd > 0) {
    close(dri_fd);
  }
  return 0;
}
