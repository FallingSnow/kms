#pragma once

#include <libdrm/drm.h>
#include <stdint.h>

struct Framebuffer {
  __u32 offsets[4];
  __s32 fds[4];
  uint64_t size;
  long width;
  long height;
  int id;
};

struct Screen {
  struct Framebuffer *buffers;
  const int num_buffers;
  int page;
  struct drm_mode_crtc crtc;
  struct drm_mode_modeinfo mode;
  struct drm_mode_get_connector connector;
};

int drm_kms(int dri_fd, struct Screen *screens);
int drm_set_mode(int dri_fd, struct Screen *screen);
int drm_swap_buffers_page_flip(int dri_fd, struct Framebuffer *fb,
                                struct drm_mode_crtc *crtc);
int drm_swap_buffers_set_crtc(int dri_fd, struct Framebuffer *fb,
                               struct drm_mode_crtc *crtc);