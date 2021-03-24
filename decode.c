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

// TODO: Dynamically pick DRI path based on capabilities
// The raspberry pi tends to switch between card0 and card1 for the rendering
// interface
#define DRI_CARD_PATH "/dev/dri/card0"
#define NUM_OUTPUT_BUFFERS 5

int read_into_buffer(void *buf, int buf_len, FILE *in_fp, int *in_offset) {
  int bytesused = read_h264_au(buf, buf_len, in_fp, *in_offset);
  if (bytesused < 1) {
    fprintf(stderr, "Unable to read h264 au\n");
    return -1;
  }
  // printf("Read %d bytes into the input buffer\n", bytesused);
  *in_offset += bytesused;

  return bytesused;
}

int buffer_dequeue_mp(int drm_fd, struct v4l2_buffer *buffer) {

  buffer->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buffer->memory = V4L2_MEMORY_DMABUF;

  printf("Buffer: idx: %d, type: %d, bytesused: %d, flags: %d, field: "
         "%d, memory: %d, len: %d, m.planes: %p\n",
         buffer->index, buffer->type, buffer->bytesused, buffer->flags,
         buffer->field, buffer->memory, buffer->length, buffer->m.planes);
  for (int p = 0; p < FMT_NUM_PLANES; p++) {
    printf(
        "Plane: idx: %d, bytesused: %d, length: %d, mem_offset: %d, fd: %d\n",
        p, buffer->m.planes[p].bytesused, buffer->m.planes[p].length,
        buffer->m.planes[p].m.mem_offset, buffer->m.planes[p].m.fd);
  }

  if (ioctl(drm_fd, VIDIOC_DQBUF, buffer) == -1) {
    perror("VIDIOC_DQBUF");
    return -1;
  }

  return 0;
}

int main() {
  int err = 0, dri_fd = 0, in_offset = 0;
  FILE *in_fp;
  struct Framebuffer fbs[NUM_OUTPUT_BUFFERS] = {0};
  struct Screen screens[1] = {{.buffers = fbs, .num_buffers = NUM_OUTPUT_BUFFERS}};
  // struct Decoder decoder = {0};
  struct perf_clock start, end;
  struct v4l2_plane planes[FMT_NUM_PLANES] = {0};
  struct v4l2_buffer buffer = {.m.planes = planes, 0};

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

  in_fp = fopen("/home/alarm/FPS_test_1080p60_L4.2.h264", "rb");
  // in_fp = fopen("/home/alarm/jellyfish-15-mbps-hd-h264.h264", "rb");
  // in_fp = fopen("/home/alarm/jellyfish-15-mbps-hd-h264.h264", "rb");
  // in_fp = fopen("/home/alarm/jellyfish-80-mbps-hd-h264.h254", "rb");
  // in_fp = fopen("/home/alarm/jellyfish-20-mbps-4k-uhd-h264-444.h264", "rb");
  if (in_fp == NULL) {
    fprintf(stderr, "Failed to open input video\n");
    return -1;
  }

  int drm_fd = init_device();
  if (drm_fd == -1) {
    fprintf(stderr, "Failed to initialize v4l2 device\n");
    return -1;
  }

  struct buffer_mp in_buffers[NUM_OUTPUT_BUFFERS] = {0};
  struct buffer_mp out_buffers[screens[0].num_buffers];
  bzero(&out_buffers, sizeof(out_buffers));

  // Link buffer_mps to screen buffers
  for (int i = 0; i < screens[0].num_buffers; i++) {
    out_buffers[i].fds[0] = screens[0].buffers[i].fds[0];
  }

  /**
   * Enqueue output buffer(s)
   */
  err = get_buffers(drm_fd, out_buffers,
                    sizeof(out_buffers) / sizeof(out_buffers[0]),
                    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
  if (err == -1) {
    fprintf(stderr, "Could not initialize v4l2 output buffers\n");
    return -1;
  }

  for (int idx = 0; idx < sizeof(out_buffers) / sizeof(out_buffers[0]); idx++) {
    bzero(&buffer, sizeof(buffer));
    bzero(&planes, sizeof(planes));
    buffer.m.planes = planes;
    init_v4l2_buffer(&buffer, idx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                     V4L2_MEMORY_DMABUF, FMT_NUM_PLANES, NULL,
                     out_buffers[idx].fds);

    // printf("Buffer: idx: %d, type: %d, bytesused: %d, flags: %d, field: "
    //        "%d, memory: %d, len: %d, m.planes: %p\n",
    //        buffer.index, buffer.type, buffer.bytesused, buffer.flags,
    //        buffer.field, buffer.memory, buffer.length, buffer.m.planes);
    // for (int p = 0; p < FMT_NUM_PLANES; p++) {
    //   printf(
    //       "Plane: idx: %d, bytesused: %d, length: %d, mem_offset: %d, fd:
    //       %d\n", p, planes[p].bytesused, planes[p].length,
    //       planes[p].m.mem_offset, planes[p].m.fd);
    // }

    if (queue_buffer(drm_fd, &buffer) < 0) {
      fprintf(stderr, "Failed to queue output buffer\n");
      return -1;
    }
  }

  /**
   * Enqueue input buffer(s)
   */

  err = get_buffers(drm_fd, in_buffers,
                    sizeof(in_buffers) / sizeof(in_buffers[0]),
                    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
  if (err == -1) {
    fprintf(stderr, "Could not initialize v4l2 input buffers\n");
    return -1;
  }

  for (int idx = 0; idx < sizeof(in_buffers) / sizeof(in_buffers[0]); idx++) {

    printf("Input Buffer %d start: %p, length: %d\n", idx,
           in_buffers[idx].start[0], in_buffers[idx].length[0]);

    bzero(&buffer, sizeof(buffer));
    bzero(&planes, sizeof(planes));
    buffer.m.planes = planes;
    init_v4l2_buffer(&buffer, idx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                     V4L2_MEMORY_MMAP, FMT_NUM_PLANES, NULL, NULL);

    // printf("Buffer: idx: %d, type: %d, bytesused: %d, flags: %d, field: "
    //        "%d, memory: %d, len: %d, m.planes: %p\n",
    //        buffer.index, buffer.type, buffer.bytesused, buffer.flags,
    //        buffer.field, buffer.memory, buffer.length, buffer.m.planes);
    // for (int p = 0; p < FMT_NUM_PLANES; p++) {
    //   printf(
    //       "Plane: idx: %d, bytesused: %d, length: %d, mem_offset: %d, fd:
    //       %d\n", p, planes[p].bytesused, planes[p].length,
    //       planes[p].m.mem_offset, planes[p].m.fd);
    // }

    // Load the input buffer
    for (int p = 0; p < FMT_NUM_PLANES; p++) {
      planes[p].bytesused =
          read_into_buffer(in_buffers[idx].start[p], in_buffers[idx].length[p],
                           in_fp, &in_offset);
    }

    if (queue_buffer(drm_fd, &buffer) < 0) {
      fprintf(stderr, "Failed to queue input buffer\n");
      return -1;
    }
  }

  if (start_stream(drm_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0) {
    fprintf(stderr, "Failed to start output stream\n");
    return -1;
  }
  if (start_stream(drm_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
    fprintf(stderr, "Failed to start capture stream\n");
    return -1;
  }

  // TODO: Check for errors
  int poll_fd = create_epoller(drm_fd);
  struct epoll_event pevents[MAX_EVENTS];

  for (;;) {
    bzero(&buffer, sizeof(buffer));
    bzero(&planes, sizeof(planes));
    buffer.m.planes = planes;

    int status = epoll_events(poll_fd, pevents);
    // printf("Poll result: %d\n", status);

    if (status < 0) {
      fprintf(stderr, "Failed to poll events!\n");
      break;
    }

    for (int i = 0; i < status; i++) {
      int event = pevents[i].events;
      // Read from V4L2 device
      if (event & EPOLLIN) {
        end = get_perf();
        display_diff_perf(&start, &end, "Decoder");
        buffer.length = FMT_NUM_PLANES;
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_DMABUF;

        if (dequeue_buffer(drm_fd, &buffer) < 0) {
          fprintf(stderr, "Failed to dequeue buffer\n");
          return -1;
        }

        printf("Buffer free: %d\n", buffer.index);
        // drm_swap_buffers_page_flip(dri_fd, &screens[0].buffers[0],
        //                            &screens[0].crtc);

        if (queue_buffer(drm_fd, &buffer) < 0) {
          fprintf(stderr, "Failed to dequeue buffer\n");
          return -1;
        }
      }
      if (event & EPOLLOUT) {
        buffer.length = FMT_NUM_PLANES;
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;

        if (dequeue_buffer(drm_fd, &buffer) < 0) {
          fprintf(stderr, "Failed to dequeue buffer\n");
          return -1;
        }

        for (int p = 0; p < FMT_NUM_PLANES; p++) {
          planes[p].bytesused = read_into_buffer(
              in_buffers[buffer.index].start[p],
              in_buffers[buffer.index].length[p], in_fp, &in_offset);
        }

        start = get_perf();
        if (queue_buffer(drm_fd, &buffer) < 0) {
          fprintf(stderr, "Failed to dequeue buffer\n");
          return -1;
        }
      }
    }
    usleep(100000);
  }

  // decoder.output = screens[0].buffers[0].ptr;
  // decoder.source = in_fp;
  // init_decoder(&decoder);
  // start_capturing(&decoder);
  // // mainloop(&decoder);
  // // run(&decoder);
  // // get_buffers(&decoder);
  // // printf("First buffer address: %p\n", decoder.buffers[0].ptr);
  // int action;
  // for (;;) {
  //   // start = get_perf();
  //   action = run(&decoder);
  //   // printf("Action %d\n", action);
  //   if (action == 1) {
  //     // Choose back buffer (Not the current buffer)
  //     int page = screens[0].page == 0 ? 1 : 0;
  //     // printf("Current page: %d\t Target page: %d\n", screen[j].page,
  //     page); struct Framebuffer *fb = &screens[0].buffers[page];
  //     // printf("Frame pointer: %lu\n", fb->size);

  //     // printf("Writing to FB: %d\n", fb->id);
  //     drm_swap_buffers_page_flip(dri_fd, fb, &screens[0].crtc);
  //     screens[0].page = page;
  //     decoder.output = screens[0].buffers[page].ptr;
  //     // end = get_perf();
  //     // display_diff_perf(&start, &end);
  //   }
  // }

  /*
   * Clean up resources
   */
  // cleanup:

  //   // Stop being the "master" of the DRI device
  //   err = ioctl(dri_fd, DRM_IOCTL_DROP_MASTER, 0);
  //   if (err) {
  //     printf("Unable to drop KMS master, status = %d, status = %s\n",
  //     err,
  //            strerror(errno));
  //     return true;
  //   }

  //   for (int i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
  //     for (int b = 0; b < NUM_BUFFERS; b++) {
  //       struct Framebuffer buffer = screens[i].buffers[b];
  //       if (buffer.ptr != NULL)
  //         munmap(buffer.ptr, buffer.size);
  //     }
  //   }
  //   if (dri_fd > 0) {
  //     close(dri_fd);
  //   }
  return 0;
}
