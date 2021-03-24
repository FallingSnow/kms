#pragma once

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEV_PATH "/dev/video10"
#define FMT_NUM_PLANES 1
#define MAX_EVENTS 4

struct buffer {
  void *ptr;
  int length;
};

struct buffer_mp {
  enum v4l2_memory type;
  union {
    void *start[FMT_NUM_PLANES];
    __s32 fds[FMT_NUM_PLANES];
  };
  unsigned int length[FMT_NUM_PLANES];
};

// struct Decoder {
//   int deviceFd;
//   struct buffer buffers[2];
//   int num_buffers;
//   struct buffer_mp *buffers_mp;
//   struct buffer_mp *buffers_mp_out;
//   unsigned int n_buffers;
//   unsigned int n_buffers_out;
//   void *source;
//   union {
//     __s32 fds[2 * FMT_NUM_PLANES];
//     void *output;
//   };
// };

int get_buffers(int drm_fd, struct buffer_mp *buffers, int n_buffers,
                enum v4l2_buf_type type);
int start_stream(int drm_fd, enum v4l2_buf_type);
int create_epoller(int drm_fd);
int epoll_events(int poller_fd, struct epoll_event pevents[MAX_EVENTS]);
int v4l2_select(int drm_fd);
int read_h264_au(void *buf, size_t buf_len, FILE *in, size_t f_offset);

// Allow you to specify optional arguments
//
// https://modelingwithdata.org/arch/00000022.htm
typedef struct {
  __u32 device_format;
  __u16 in_height;
  __u16 in_width;
  __u16 out_height;
  __u16 out_width;
} init_device_args;
int _init_device_base(init_device_args args);
#define init_device(...) _init_device_base((init_device_args){__VA_ARGS__});

void init_v4l2_buffer(struct v4l2_buffer *buffer, int index,
                      enum v4l2_buf_type type, enum v4l2_memory mtype,
                      int num_planes, unsigned int *lengths, __s32 *dmabuf_fds);
int queue_buffer(int drm_fd, struct v4l2_buffer *buffer);
int dequeue_buffer(int drm_fd, struct v4l2_buffer *buffer);
int query_buffer(int drm_fd, struct v4l2_buffer *buffer);
