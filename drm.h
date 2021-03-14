#pragma once

#include <libdrm/drm.h>
#include <stdint.h>
#include <stdbool.h>

#define NUM_BUFFERS 2

struct Framebuffer {
  __u32 offsets[4];
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
  struct drm_mode_modeinfo mode;
  struct drm_mode_get_connector connector;
};

int drm_kms(int dri_fd, struct Screen *screens);
int drm_set_mode(int dri_fd, struct Screen *screen);
bool drm_swap_buffers_page_flip(int dri_fd, struct Framebuffer *fb,
                                struct drm_mode_crtc *crtc);
bool drm_swap_buffers_set_crtc(int dri_fd, struct Framebuffer *fb,
                               struct drm_mode_crtc *crtc);