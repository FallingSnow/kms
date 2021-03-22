#define _GNU_SOURCE
#include "v4l2.h"
#include "util.h"
#include <assert.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void init_v4l2_buffer(struct v4l2_buffer *buffer, int index,
                      enum v4l2_buf_type type, enum v4l2_memory mtype,
                      int num_planes, unsigned int *lengths,
                      __s32 *dmabuf_fds) {
  assert(buffer != NULL);

  buffer->index = index;
  buffer->type = type;
  buffer->memory = mtype;

  if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
    assert(buffer->m.planes != NULL);
    buffer->length = num_planes;
    // if (lengths != NULL)
    //   for (int p = 0; p < num_planes; p++) {
    //     buffer->m.planes[p].length = lengths[p];
    //   }
  }

  if (mtype == V4L2_MEMORY_DMABUF && dmabuf_fds != NULL) {
    if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
      for (int p = 0; p < num_planes; p++) {
        assert(dmabuf_fds[p] != 0);
        buffer->m.planes[p].m.fd = dmabuf_fds[p];
      }
    } else {
      buffer->m.fd = dmabuf_fds[0];
    }
  }

  // if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
  //   for (int p = 0; p < num_planes; p++)
  //     DEBUG("\tPlane: %d, Start: %d, FD: %d\n", p,
  //           buffer->m.planes[p].m.mem_offset, buffer->m.planes[p].m.fd);
  // }
}

//------------------------------------------------------------------------------
// Queue Buffer
//------------------------------------------------------------------------------

// The actual qeue_buffer function
int queue_buffer(int drm_fd, struct v4l2_buffer *buffer) {

  // Queues buffer for decoding.
  if (-1 == xioctl(drm_fd, VIDIOC_QBUF, buffer)) {
    fprintf(stderr, "Unable to queue buffer, %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Dequeue Buffer
//------------------------------------------------------------------------------

int dequeue_buffer(int drm_fd, struct v4l2_buffer *buffer) {

  if (-1 == xioctl(drm_fd, VIDIOC_DQBUF, &buffer)) {
    switch (errno) {
    case EAGAIN:
      return 0;

    case EIO:
      /* Could ignore EIO (Internal Error), see spec. */

      /* fall through */

    default:
      fprintf(stderr, "Unable to dequeue buffer, %s\n", strerror(errno));
      return -1;
    }
  }

  return 0;
}

int query_buffer(int drm_fd, struct v4l2_buffer *buffer) {
  if (-1 == xioctl(drm_fd, VIDIOC_QUERYBUF, buffer)) {
    fprintf(stderr, "Unable to query buffer, %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

//------------------------------------------------------------------------------
// __Request Buffers from device__
// For mmap buffers we need to register them with the V4L2 device and mmap the
// resulting device memory.
//
// For dmabuf buffers we need to just register them with the V4L2 device.
//------------------------------------------------------------------------------
int get_buffers(int drm_fd, struct buffer_mp *buffers, int n_buffers,
                enum v4l2_buf_type type) {
  // Use DMABUF for output (v4l2 device writes to DMABUF, aka capture)
  enum v4l2_memory memory_type = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                                     ? V4L2_MEMORY_MMAP
                                     : V4L2_MEMORY_DMABUF;

  // Request some buffers on the device
  struct v4l2_requestbuffers req = {
      .count = n_buffers, .type = type, .memory = memory_type, 0};

  // DEBUG("Request buffers capabilities: %d\n", req.capabilities);

  // Make the request
  if (-1 == xioctl(drm_fd, VIDIOC_REQBUFS, &req)) {
    fprintf(stderr, "Could not request buffers, %s\n", strerror(errno));
    return -1;
  }

  // Check if we received less buffers than we asked for
  if (req.count < n_buffers) {
    fprintf(stderr, "Insufficient device buffer memory\n");
    return -1;
  }

  // For each buffer on the device, we are going to mmap it so we can access it
  // from userspace
  for (size_t idx = 0; idx < req.count; idx++) {
    struct v4l2_plane planes[FMT_NUM_PLANES] = {0};
    struct v4l2_buffer buffer = {.m.planes = planes, 0};

    buffers[idx].type = memory_type;
    init_v4l2_buffer(&buffer, idx, type, memory_type, FMT_NUM_PLANES,
                     buffers[idx].length, buffers[idx].fds);

    if (query_buffer(drm_fd, &buffer) < 0) {
      fprintf(stderr, "Failed to query buffer\n");
      return -1;
    }

    printf(
        "[%zu] Buffer: idx: %d, type: %d, bytesused: %d, flags: %d, field: "
        "%d, memory: %d, len: %d, request_fd: %d, m.planes: %p\n",
        idx, buffer.index, buffer.type, buffer.bytesused, buffer.flags,
        buffer.field, buffer.memory, buffer.length, buffer.request_fd, buffer.m.planes);

    if (V4L2_TYPE_IS_MULTIPLANAR(type))
      for (int i = 0; i < FMT_NUM_PLANES; i++) {
        printf("[%zu][%d] Plane fd: %d, bytesused: %d, length: %d, mem_offset: "
               "%d, fd: %d\n",
               idx, i, buffer.m.planes[i].m.fd, buffer.m.planes[i].bytesused,
               buffer.m.planes[i].length, buffer.m.planes[i].m.mem_offset,
               buffer.m.planes[i].m.fd);
      }

    for (int p = 0; p < FMT_NUM_PLANES; p++) {
      buffers[idx].length[p] = buffer.length;

      // MMAP buffers are allocated by the kernel so we get their address via a
      // mmap
      //
      // DMABUF buffers on the other hand are already allocated before you even
      // call this function, so there is no need to mmap them (because really
      // they live in userland anyway)
      if (memory_type == V4L2_MEMORY_MMAP) {
        buffers[idx].length[p] = buffer.m.planes[p].length;
        buffers[idx].start[p] =
            mmap(NULL, buffer.m.planes[p].length, PROT_READ | PROT_WRITE,
                 MAP_SHARED, drm_fd, buffer.m.planes[p].m.mem_offset);

        if (buffers[idx].start[p] == MAP_FAILED) {
          fprintf(stderr, "Could not mmap buffer %zu\n", idx);
          return -1;
        }
      }
    }
  }

  return req.count;
}

//------------------------------------------------------------------------------
// Initialize v4l2 Device
//------------------------------------------------------------------------------

static int set_format(int drm_fd, struct v4l2_format *format) {
  if (-1 == xioctl(drm_fd, VIDIOC_S_FMT, format)) {
    fprintf(stderr, "Failed VIDIOC_S_FMT, %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

static int check_device_capabilities(int drm_fd) {
  struct v4l2_capability capability = {0};

  if (xioctl(drm_fd, VIDIOC_QUERYCAP, &capability) < 0) {
    fprintf(stderr, "Unable to query video device capabilities, %s\n",
            strerror(errno));
    return -1;
  }

  if (!(capability.capabilities &
        (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE |
         V4L2_CAP_VIDEO_CAPTURE))) {
    fprintf(stderr, "Device does not support video capture\n");
    return -1;
  }
  if (!(capability.capabilities & V4L2_CAP_STREAMING)) {
    fprintf(stderr, "Device does not support a streaming interface\n");
    return -1;
  }
  return 0;
}

int init_device_base(__u32 device_format, __u16 in_height, __u16 in_width,
                     __u16 out_height, __u16 out_width) {
  struct v4l2_format format = {0};

  int drm_fd = open(DEV_PATH, O_RDWR);

  if (check_device_capabilities(drm_fd) < 0) {
    return -1;
  }

  // Set input format
  format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  format.fmt.pix_mp.width = in_width;
  format.fmt.pix_mp.height = in_height;
  format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
  format.fmt.pix_mp.field = V4L2_FIELD_ANY;
  if (set_format(drm_fd, &format) < 0) {
    return -1;
  }

  // Set output format
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  format.fmt.pix_mp.width = out_width;
  format.fmt.pix_mp.height = out_height;
  format.fmt.pix_mp.pixelformat = device_format;
  format.fmt.pix_mp.field = V4L2_FIELD_NONE;
  if (set_format(drm_fd, &format) < 0) {
    return -1;
  }

  // TODO: Handle source change and EOS (End of Stream) events

  // struct v4l2_event_subscription sub = {0};

  // sub.type = V4L2_EVENT_EOS;
  // if (-1 == ioctl(drm_fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) {
  //   fprintf(stderr, "Failed VIDIOC_SUBSCRIBE_EVENT, %s\n", strerror(errno));
  //   return -1;
  // }

  // sub.type = V4L2_EVENT_SOURCE_CHANGE;
  // if (-1 == ioctl(drm_fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) {
  //   fprintf(stderr, "Failed VIDIOC_SUBSCRIBE_EVENT, %s\n", strerror(errno));
  //   return -1;
  // }

  return drm_fd;
}

// Setup default parameters
int _init_device_base(init_device_args args) {

  args.device_format =
      args.device_format ? args.device_format : V4L2_PIX_FMT_RGB565;
  args.in_height = args.in_height ? args.in_height : 1080;
  args.in_width = args.in_width ? args.in_width : 1920;
  args.out_height = args.out_height ? args.out_height : 1080;
  args.out_width = args.out_width ? args.out_width : 1920;

  int result = init_device_base(args.device_format, args.in_height,
                                args.in_width, args.out_height, args.out_width);
  return result;
}

//------------------------------------------------------------------------------
// Start the V4L2 interface
//------------------------------------------------------------------------------

int start_stream(int drm_fd, enum v4l2_buf_type type) {
  if (-1 == xioctl(drm_fd, VIDIOC_STREAMON, &type)) {
    fprintf(stderr, "VIDIOC_STREAMON failed, %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Poll for events
//
// There are 2 ways of polling, using select or using epoll. Currently only
// select works.
//------------------------------------------------------------------------------

int create_epoller(int drm_fd) {
  struct epoll_event ev;

  int polling_fd = epoll_create(1);

  if (polling_fd < 0) {
    fprintf(stderr, "Unable to create epoll interface\n");
    return -1;
  }

  ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  ev.data.fd = drm_fd;

  if (epoll_ctl(polling_fd, EPOLL_CTL_ADD, drm_fd, &ev) != 0) {
    fprintf(stderr, "Unable to add fd to epoll, status = %s\n",
            strerror(errno));
    return -1;
  }

  return polling_fd;
}

static int handle_event(drm_fd) {
  struct v4l2_event ev = {0};

  // do {
  if (ioctl(drm_fd, VIDIOC_DQEVENT, &ev) == -1) {
    fprintf(stderr, "Failed VIDIOC_DQEVENT, %s\n", strerror(errno));
    return -1;
  }
  switch (ev.type) {
  case V4L2_EVENT_SOURCE_CHANGE:
    printf("Source changed\n");
    return 12;
  case V4L2_EVENT_EOS:
    fprintf(stderr, "EOS\n");
    return 11;
  default:
    fprintf(stderr, "Unknown event code = %d\n", ev.type);
    return -1;
  }
  // } while (ev.pending > 0);

  return 0;
}

int epoll_events(int poller_fd) {
  struct epoll_event pevents[MAX_EVENTS] = {0};
  // printf("EPOLL waiting for poller %d\n", poller->fd);
  int ready = epoll_wait(poller_fd, pevents, MAX_EVENTS, 10000);
  printf("Num events %d\n", ready);

  // Check if epoll actually succeeded
  if (ready == -1) {
    fprintf(stderr, "epoll failed, %s\n", strerror(errno));
    return -1;
  }
  // report error and abort
  else if (ready == 0) {
    fprintf(stderr, "epoll timed out\n");
    return -1;
  }

  // Check if any events detected
  for (int i = 0; i < ready; i++) {
    printf("Poll event %d: %d\n", i, pevents[i].events);
    if (pevents[i].events & EPOLLIN) {
      return 1;
    }
    if (pevents[i].events & EPOLLOUT) {
      return 2;
    }
    if (pevents[i].events & EPOLLERR) {
      fprintf(stderr, "epoll event error\n");
      return handle_event(pevents[i].data.fd);
    }
    printf("Unhandled poll event %d\n", pevents[i].events);
  }

  return 0;
}

int v4l2_select(int drm_fd) {
  fd_set fds[3];
  fd_set *rd_fds = &fds[0]; /* for capture */
  fd_set *ex_fds = &fds[1]; /* for capture */
  fd_set *wr_fds = &fds[2]; /* for output */
  struct timeval tv;
  int r;

  if (rd_fds) {
    FD_ZERO(rd_fds);
    FD_SET(drm_fd, rd_fds);
  }

  if (ex_fds) {
    FD_ZERO(ex_fds);
    FD_SET(drm_fd, ex_fds);
  }

  if (wr_fds) {
    FD_ZERO(wr_fds);
    FD_SET(drm_fd, wr_fds);
  }

  /* Timeout. */
  tv.tv_sec = 10;
  tv.tv_usec = 0;

  r = select(drm_fd + 1, rd_fds, wr_fds, ex_fds, &tv);

  if (-1 == r) {
    if (EINTR == errno)
      return 0;
    fprintf(stderr, "Select failed, %s\n", strerror(errno));
    return -1;
  }

  if (0 == r) {
    printf("select timeout\n");
    return -1;
  }

  if (rd_fds && FD_ISSET(drm_fd, rd_fds)) {
    printf("Reading!\n");
    return 1;
  }
  if (wr_fds && FD_ISSET(drm_fd, wr_fds)) {
    printf("Writing!\n");
    return 2;
  }
  if (ex_fds && FD_ISSET(drm_fd, ex_fds)) {
    fprintf(stderr, "Exception\n");
    if (handle_event(drm_fd)) {
      return -1;
    }
    printf("Event handled\n");
  }
  return 0;
}

//------------------------------------------------------------------------------
// Read a Access Unit delimited by Access Unit Delemeters from a file
//------------------------------------------------------------------------------
int read_h264_au(void *buf, size_t buf_len, FILE *in, size_t f_offset) {
  unsigned char aud[4] = {0, 0, 0, 1}; // Start sequence for a h264 AUD,
                                       // denotes beginning of a single frame
  size_t au_length, bytesused;
  void *b_offset;

  if (-1 == fseek(in, f_offset, SEEK_SET)) {
    fprintf(stderr, "Failed to seek in input file\n");
    return -1;
  }
  // OPTIMIZE: We shouldn't reread, instead we should mask the data passed to
  // the buffer. Maybe use a buffered reader?
  bytesused = fread(buf, 1, buf_len, in);
  if (bytesused != buf_len) {
    printf("EOF of input reached\n");
    return -1;
  }

  // printf("Searching for AUD in %p. [Buf: %p, read: %u]\n", in_fp, buf,
  //        *bytesused);
  // printf("Used %u bytes. First 8 bytes %02x %02x %02x %02x\n", *bytesused,
  //        buf_char[0], buf_char[1], buf_char[2], buf_char[3]);

  // OPTIMIZE: Upgrade memmem to https://github.com/mischasan/aho-corasick
  //
  // Find the next Access Unit Delimiter (0x00000001) in the byte stream
  b_offset = memmem(buf + 1, bytesused, aud, sizeof(aud) / sizeof(aud[0]));
  // b_offset = strstr_last(buf + 1, *bytesused, (const char *)aud,
  //                        sizeof(aud) / sizeof(aud[0]));
  if (b_offset == NULL) {
    fprintf(stderr, "Next AUD not found!\n");
    return 0;
  }

  au_length = b_offset - buf;

  // Clear AUD
  // printf("AUD found at %p, Setting %lu bytes to 0\n", b_offset,
  //        buf_len - au_length);
  memset(b_offset, 0, bytesused - au_length);
  bytesused = au_length;
  f_offset += au_length + 1;

  return bytesused;
}