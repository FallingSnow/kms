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

//------------------------------------------------------------------------------
// Kernel Mode Setting (KMS)
//------------------------------------------------------------------------------
int kms(int dri_fd, struct drm_mode_get_connector *conn, struct drm_mode_get_encoder *enc) {
  int err = 0;

  printf("connection: %d -- mode: %d, prop: %d, enc: %d\n", conn.connection,
         conn.count_modes, conn.count_props, conn.count_encoders);
  printf("modes: %dx%d\n", conn_mode_buf[0].hdisplay,
         conn_mode_buf[0].vdisplay);

  enc.encoder_id = conn.encoder_id;
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETENCODER, &enc); // get encoder
  if (err) {
    printf("Unable to get encoder, status = %d, status = %s\n", err,
           strerror(errno));
    return err;
  }

  struct drm_mode_crtc *crtc = &screen[i].crtc;
  crtc->crtc_id = enc.crtc_id;
  err = ioctl(dri_fd, DRM_IOCTL_MODE_GETCRTC, &crtc); // get crtc
  if (err) {
    printf("Unable to get crtc, status = %d, status = %s\n", err,
           strerror(errno));
  }

  crtc->fb_id = screen[i].buffers[0].id;
  crtc->set_connectors_ptr = (uint64_t)&res_conn_buf[i];
  crtc->count_connectors = 1;
  crtc->mode = conn_mode_buf[0];
  crtc->mode_valid = 1;
  err = ioctl(dri_fd, DRM_IOCTL_MODE_SETCRTC, crtc); // get crtc
  if (err) {
    printf("Unable to set crtc, status = %d, status = %s\n", err,
           strerror(errno));
  }

  printf("Set crtc mode\n");
  num_screens += 1;
}

printf("Finished creating DRM buffers\n");
}