#include <errno.h>
#include <libdrm/drm_fourcc.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
// #include <asm-generic/fcntl.h>

#include "drm.h"

// OPTIMIZE: Map the dumb buffer to a frame buffer
// https://www.systutorials.com/docs/linux/man/7-drm-memory/

// OPTIMIZE: Use driver buffers rather than dumb buffers

// OPTIMIZE: Use dma-prime file descriptors
// https://gist.github.com/Miouyouyou/2f227fd9d4116189625f501c0dcf0542

// Most of this code is from https://github.com/6by9/v4l2_m2m

/**
 * Wait for next VSync, DRM method.
 *
 * Stolen from:
 * https://github.com/MythTV/mythtv/blob/master/mythtv/libs/libmythtv/vsync.cpp
 */
static int vsync_drm_wait(int drm_fd, unsigned int target) {
  int ret = -1;
  drm_wait_vblank_t vbl;

  vbl.request.type = _DRM_VBLANK_RELATIVE;
  vbl.request.sequence = target;

  do {
    ret = ioctl(drm_fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
    vbl.request.type &= ~_DRM_VBLANK_RELATIVE;
  } while (ret && errno == EINTR);

  if (ret)
    fprintf(stderr,
            "vsync_drm_wait(): VBlank ioctl did not work, unimplemented in "
            "this drmver? status = %s\n",
            strerror(errno));

  return ret;
}

int drm_swap_buffers_page_flip(int dri_fd, struct Framebuffer *fb,
                                struct drm_mode_crtc *crtc) {
  // printf("Flipping page\n");
  struct drm_mode_crtc_page_flip_target flip_target = {0};
  int err = 0;

  flip_target.crtc_id = crtc->crtc_id;
  flip_target.fb_id = fb->id;
  // flip_target.flags = DRM_MODE_PAGE_FLIP_ASYNC;
  // printf("Flipping page to %d\n", fb->id);

  // Since we schedules a page flip we need to wait for it to actually happen
  // vsync_drm_wait(dri_fd);
  err = ioctl(dri_fd, DRM_IOCTL_MODE_PAGE_FLIP,
              &flip_target); // request a page flip
  if (err < 0) {

    if (errno == EBUSY) {
      printf("Buffer page flip already pending. Skipping.\n");
      return -1;
      vsync_drm_wait(dri_fd, 1);

      return drm_swap_buffers_page_flip(dri_fd, fb, crtc);
    }

    fprintf(stderr, "Error requesting page flip, err = %d, status = %s\n", err,
            strerror(errno));
    return -1;
  }

  return 0;
}

int drm_swap_buffers_set_crtc(int dri_fd, struct Framebuffer *fb,
                               struct drm_mode_crtc *crtc) {
  int err;
  crtc->fb_id = fb->id;

  err = ioctl(dri_fd, DRM_IOCTL_SET_MASTER, 0);
  if (err < 0) {
    fprintf(stderr,
            "Error getting KMS master status, status = %d, status = %s\n", err,
            strerror(errno));
    return -1;
  }

  err = ioctl(dri_fd, DRM_IOCTL_MODE_SETCRTC, crtc); // get crtc
  if (err) {
    fprintf(stderr, "Unable to set crtc, status = %d, status = %s\n", err,
            strerror(errno));
    return -1;
  }

  // Stop being the "master" of the DRI device
  err = ioctl(dri_fd, DRM_IOCTL_DROP_MASTER, 0);
  if (err) {
    fprintf(stderr, "Unable to drop KMS master, status = %d, status = %s\n",
            err, strerror(errno));
    return -1;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Mode setting
//------------------------------------------------------------------------------
int drm_set_mode(int dri_fd, struct Screen *screen) {
  struct drm_mode_get_encoder enc = {0};
  int err = 0;

  //   printf("connection: %d -- mode: %d, prop: %d, enc: %d\n",
  //   conn->connection,
  //          conn->count_modes, conn->count_props, conn->count_encoders);
  //   printf("modes: %dx%d\n", conn_mode_buf[0].hdisplay,
  //          conn_mode_buf[0].vdisplay);

  enc.encoder_id = screen->connector.encoder_id;
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETENCODER, &enc); // get encoder
  if (err) {
    fprintf(stderr, "Unable to get encoder, status = %d, status = %s\n", err,
            strerror(errno));
    return err;
  }

  struct drm_mode_crtc *crtc = &screen->crtc;
  crtc->crtc_id = enc.crtc_id;
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETCRTC, &crtc); // get crtc
  if (err) {
    printf("Warning: Unable to get current crtc, status = %d, status = %s\n",
           err, strerror(errno));
  }

  crtc->fb_id = screen->buffers[0].id;
  crtc->set_connectors_ptr = (uint64_t)&screen->connector.connector_id;
  crtc->count_connectors = 1;
  crtc->mode = screen->mode;
  crtc->mode_valid = 1;
  err = ioctl(dri_fd, DRM_IOCTL_MODE_SETCRTC, crtc); // get crtc
  if (err) {
    fprintf(stderr, "Unable to set crtc, status = %d, status = %s\n", err,
            strerror(errno));
    return err;
  }

  printf("Set crtc mode\n");

  return 0;
}

//------------------------------------------------------------------------------
// Creating a dumb buffer
//------------------------------------------------------------------------------
int create_buffers(int dri_fd, struct drm_mode_modeinfo *conn_mode_buf,
                   struct Framebuffer *buffers, int num_buffers) {
  // We create NUM_BUFFERS buffers for each screen. These are swapped out
  // during after a vblank.

  struct drm_mode_create_dumb create_dumb = {0};
  struct drm_mode_fb_cmd2 cmd_dumb = {0};
  struct drm_prime_handle prime = {0};
  int err;

  for (int b = 0; b < num_buffers; b++) {
    struct Framebuffer *buffer = &buffers[b];
    // If we create the buffer later, we can get the size of the screen first.
    // This must be a valid mode, so it's probably best to do this after we
    // find a valid crtc with modes.
    create_dumb.width = conn_mode_buf->hdisplay;
    // TODO: Find out why v4l2 and drm disagree about what size the buffer
    // should be, thats why the + 10 is here
    create_dumb.height = conn_mode_buf->vdisplay + 10;
    create_dumb.bpp = 16;
    err = ioctl(dri_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
    if (err) {
      printf("Failed to create dumb buffer, status = %d, status = %s\n", err,
             strerror(errno));
      return err;
    }

    cmd_dumb.width = create_dumb.width;
    cmd_dumb.height = create_dumb.height;
    cmd_dumb.pixel_format = DRM_FORMAT_RGB565;
    cmd_dumb.handles[0] = create_dumb.handle;
    cmd_dumb.pitches[0] = create_dumb.pitch;
    // cmd_dumb.offsets[1] = 4;
    printf("buffer pitch: %d = %d %d\n", create_dumb.pitch, cmd_dumb.pitches[0],
           cmd_dumb.pitches[1]);
    // cmd_dumb.bpp = create_dumb.bpp;
    // cmd_dumb.pitch = create_dumb.pitch;
    // cmd_dumb.depth = 24;
    // cmd_dumb.handle = create_dumb.handle;
    err = ioctl(dri_fd, DRM_IOCTL_MODE_ADDFB2, &cmd_dumb);
    if (err) {
      printf("Failed to add frame buffer, status = %d, status = %s\n", err,
             strerror(errno));
      return err;
    }

    printf("Created dumb buffer %d of %llu bytes\n", cmd_dumb.fb_id,
           create_dumb.size);

    // printf("Offsets %d %d %d\n", cmd_dumb.offsets[0], cmd_dumb.offsets[1],
    //        cmd_dumb.offsets[2]);
    buffer->offsets[0] = cmd_dumb.offsets[0];
    buffer->size = create_dumb.size;
    buffer->width = create_dumb.width;
    buffer->height = create_dumb.height;
    buffer->id = cmd_dumb.fb_id;

    prime.handle = create_dumb.handle;
    // prime.flags = DRM_CLOEXEC | DRM_RDWR;
    err = ioctl(dri_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
    if (err) {
      printf(
          "Failed to create prime file descriptor, status = %d, status = %s\n",
          err, strerror(errno));
      return err;
    }

    // printf("Got prime FD %d\n", prime.fd);

    // Allocate buffer wrappers for v4l2
    buffer->fds[0] = prime.fd;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Kernel Mode Setting (KMS)
//------------------------------------------------------------------------------

int drm_kms(int dri_fd, struct Screen *screens) {
  int err = 0;
  struct drm_mode_card_res card_resources = {0};
  uint64_t res_fb_buf[10] = {0}, res_crtc_buf[10] = {0}, res_conn_buf[10] = {0},
           res_enc_buf[10] = {0};
  struct drm_mode_modeinfo conn_mode_buf[50] = {0};
  uint64_t conn_prop_buf[50] = {0}, conn_propval_buf[50] = {0},
           conn_enc_buf[50] = {0};

  err = ioctl(dri_fd, DRM_IOCTL_SET_MASTER, 0);
  if (err < 0) {
    printf("Error getting KMS master status, status = %d, status = %s\n", err,
           strerror(errno));
    return -1;
  }

  // Get resource counts
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETRESOURCES, &card_resources);
  if (err < 0) {
    printf("Error getting mode resources, status = %d, status = %s\n", err,
           strerror(errno));
    return -1;
  }

  card_resources.fb_id_ptr = (uint64_t)res_fb_buf;
  card_resources.crtc_id_ptr = (uint64_t)res_crtc_buf;
  card_resources.connector_id_ptr = (uint64_t)res_conn_buf;
  card_resources.encoder_id_ptr = (uint64_t)res_enc_buf;

  // Get resource IDs
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETRESOURCES, &card_resources);
  if (err < 0) {
    printf("Error getting mode resources ids, status = %d, status = %s\n", err,
           strerror(errno));
    return -1;
  }

  //   printf("%s has %d connectors\n", DRI_CARD_PATH,
  //          card_resources.count_connectors);

  int i;
  for (i = 0; i < card_resources.count_connectors; i++) {
    struct Screen *screen = screens + i;

    screen->connector.connector_id = res_conn_buf[i];
    printf("Set connector id to %u\n", screen->connector.connector_id);

    printf("Getting connector %d\n", i);
    err = ioctl(dri_fd, DRM_IOCTL_MODE_GETCONNECTOR,
                &screen->connector); // get connector resource counts
    if (err) {
      printf("Unable to get connector %d, status = %d\n", i, err);
      continue;
    }
    screen->connector.modes_ptr = (uint64_t)conn_mode_buf;
    screen->connector.props_ptr = (uint64_t)conn_prop_buf;
    screen->connector.prop_values_ptr = (uint64_t)conn_propval_buf;
    screen->connector.encoders_ptr = (uint64_t)conn_enc_buf;

    printf("[#%d] Getting connector resource ids\n",
           screen->connector.connector_id);
    err = ioctl(dri_fd, DRM_IOCTL_MODE_GETCONNECTOR,
                &screen->connector); // get connector resource IDs
    if (err) {
      printf("Failed to get connector ids, status = %d, status = %s\n", err,
             strerror(errno));
    }

    if (screen->connector.count_encoders < 1 ||
        screen->connector.count_modes < 1 || !screen->connector.encoder_id ||
        !screen->connector.connection) {
      printf("[#%d] encoders: %d\tmodes: %d\n", screen->connector.connector_id,
             screen->connector.count_encoders, screen->connector.count_modes);
      continue;
    }
    for (int c = 0; c < screen->connector.count_modes; c++) {
      printf("[#%d] Modes: %dx%d@%d\n", screen->connector.connector_id,
             conn_mode_buf[c].hdisplay, conn_mode_buf[c].vdisplay,
             conn_mode_buf[c].vrefresh);
    }
    printf("[#%d] Using mode: %dx%d@%d\n", screen->connector.connector_id,
           conn_mode_buf[0].hdisplay, conn_mode_buf[0].vdisplay,
           conn_mode_buf[0].vrefresh);

    screen->mode = conn_mode_buf[0];

    err = create_buffers(dri_fd, &screen->mode, screen->buffers, screen->num_buffers);
    if (err) {
      printf("Failed to create buffers, status = %d\n", err);
      return -1;
    }
  }

  return 0;
}