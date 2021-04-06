#include <bits/stdint-uintn.h>
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <netinet/in.h> // for sockaddr_in, ntohs, htonl, htons
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h> // for bind, recvfrom, socket, AF_INET
#include <unistd.h>

#include "drm.h"
#include "safe_queue.h"
#include "socket.h"
#include "util.h"
#include "v4l2.h"

// TODO: Dynamically pick DRI path based on capabilities
// The raspberry pi tends to switch between card0 and card1 for the rendering
// interface
#define DRI_CARD_PATH "/dev/dri/card0"
#define NUM_INPUT_BUFFERS 10
#define NUM_OUTPUT_BUFFERS 2
#define AUD_READ_SIZE 5

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

int handle_drm_event_file(struct epoll_event *event, struct Screen *screen,
                          struct buffer_mp *in_buffers,
                          struct buffer_mp *out_buffers, int dri_fd,
                          FILE *in_fp, int *in_offset) {
  struct v4l2_plane planes[FMT_NUM_PLANES] = {0};
  struct v4l2_buffer buffer = {.m.planes = planes, 0};

  // Read from V4L2 device
  if (event->events & EPOLLIN) {
    buffer.length = FMT_NUM_PLANES;
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_DMABUF;
    int past_page = screen->page;

    if (dequeue_buffer(event->data.fd, &buffer) < 0) {
      fprintf(stderr, "Failed to dequeue buffer\n");
      return -1;
    }

    // out_buffers[buffer.index].fds[0];
    drm_swap_buffers_page_flip(dri_fd, screen->buffers + buffer.index,
                               &screen->crtc);
    screen->page = buffer.index;
    // printf("Swapped buffer %d for %d\n",
    // screens[0].buffers[past_page].fds[0],
    // screens[0].buffers[buffer.index].fds[0]);

    bzero(&buffer, sizeof(buffer));
    bzero(&planes, sizeof(planes));
    buffer.m.planes = planes;
    init_v4l2_buffer(&buffer, past_page, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                     V4L2_MEMORY_DMABUF, FMT_NUM_PLANES, NULL,
                     out_buffers[past_page].fds);

    if (queue_buffer(event->data.fd, &buffer) < 0) {
      fprintf(stderr, "Failed to dequeue buffer\n");
      return -1;
    }
  }
  if (event->events & EPOLLOUT) {
    buffer.length = FMT_NUM_PLANES;
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;

    if (dequeue_buffer(event->data.fd, &buffer) < 0) {
      fprintf(stderr, "Failed to dequeue buffer\n");
      return -1;
    }

    for (int p = 0; p < FMT_NUM_PLANES; p++) {
      planes[p].bytesused = read_into_buffer(in_buffers[buffer.index].start[p],
                                             in_buffers[buffer.index].length[p],
                                             in_fp, in_offset);
    }

    if (queue_buffer(event->data.fd, &buffer) < 0) {
      fprintf(stderr, "Failed to dequeue buffer\n");
      return -1;
    }
  }

  return 0;
}

int handle_drm_event_udp(struct epoll_event *event, struct Screen *screen,
                         struct buffer_mp *in_buffers,
                         struct buffer_mp *out_buffers, int dri_fd,
                         queue_t *free_buffers) {
  struct v4l2_plane planes[FMT_NUM_PLANES] = {0};
  struct v4l2_buffer buffer = {.m.planes = planes, 0};

  // Read from V4L2 device
  if (event->events & EPOLLIN) {
    buffer.length = FMT_NUM_PLANES;
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_DMABUF;
    int past_page = screen->page;

    if (dequeue_buffer(event->data.fd, &buffer) < 0) {
      fprintf(stderr, "Failed to dequeue buffer\n");
      return -1;
    }

    // out_buffers[buffer.index].fds[0];
    drm_swap_buffers_page_flip(dri_fd, screen->buffers + buffer.index,
                               &screen->crtc);
    screen->page = buffer.index;
    // printf("Swapped buffer %d for %d\n",
    // screens[0].buffers[past_page].fds[0],
    // screens[0].buffers[buffer.index].fds[0]);

    bzero(&buffer, sizeof(buffer));
    bzero(&planes, sizeof(planes));
    buffer.m.planes = planes;
    init_v4l2_buffer(&buffer, past_page, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                     V4L2_MEMORY_DMABUF, FMT_NUM_PLANES, NULL,
                     out_buffers[past_page].fds);

    if (queue_buffer(event->data.fd, &buffer) < 0) {
      fprintf(stderr, "Failed to queue capture buffer into v4l2 device\n");
      return -1;
    }
  }
  if (event->events & EPOLLOUT) {
    buffer.length = FMT_NUM_PLANES;
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;

    if (dequeue_buffer(event->data.fd, &buffer) < 0) {
      fprintf(stderr, "Failed to dequeue buffer\n");
      return -1;
    }

    int err = queue_write(free_buffers, in_buffers + buffer.index);
    if (err) {
      fprintf(stderr, "Unable to write to free buffer queue\n");
      return err;
    }
    printf("Wrote buffer to free queue, queue size = %d\n",
           queue_size(free_buffers));
  }

  return 0;
}

int handle_udp_event(struct epoll_event *event, struct Queue *free_buffers) {
  const char aud[4] = {0, 0, 0, 1}, read_aud[AUD_READ_SIZE] = {0};
  struct sockaddr_in targetaddr = {0};
  socklen_t targetaddrLength = sizeof(targetaddr);
  int bytes_read = 0, max_size = 0;
  char *buffer_start = NULL;
  struct buffer_mp *buffer = NULL;

  buffer = queue_peek(free_buffers);
  if (buffer == NULL) {
    printf("No buffer in free_buffers queue\n");
    return -1;
  }
  printf("udp event, queue_size = %d\n", queue_size(free_buffers));
  // printf("event buffer! size: %d, type: %d, into: %p, max: %d\n",
  //        buffer->size[0], buffer->type, buffer_start, max_size);
  buffer_start = buffer->start[0] + buffer->size[0];
  max_size = buffer->length[0] - buffer->size[0];

  // Read the first 4 bytes in the packet
  bytes_read += recvfrom(event->data.fd, (void *)read_aud, AUD_READ_SIZE, 0,
                         (struct sockaddr *)&targetaddr, &targetaddrLength);
  if (bytes_read < 0) {
    fprintf(stderr, "Unable to read socket device file descriptor\n");
    return -1;
  }

  int nal_unit_type = read_aud[4] & 0b00011111;

  // Check if the first 4 in this packet are a H264 AUD
  int eq = memcmp(aud, read_aud, 4);

  // If this buffer is already in use and we got a AUD, skip to next buffer
  if (buffer->size > 0 && eq == 0 /*&& nal_unit_type < 6*/) {
    buffer = queue_peek_next(free_buffers);
    if (buffer == NULL) {
      printf("No next buffer in free_buffers queue\n");
      return -1;
    }

    buffer_start = buffer->start[0] + buffer->size[0];
    max_size = buffer->length[0] - buffer->size[0];
  }

  memcpy(buffer, buffer_start, AUD_READ_SIZE);
  buffer_start += AUD_READ_SIZE;

  buffer->size[0] += bytes_read =
      recvfrom(event->data.fd, buffer_start, max_size, 0,
               (struct sockaddr *)&targetaddr, &targetaddrLength);

  if (bytes_read < 0) {
    fprintf(stderr, "Unable to read socket device file descriptor\n");
    return -1;
  }

  return eq == 0;
}

int main() {
  int err = 0, dri_fd = 0, in_offset = 0, udp_socket_fd = 0;
  FILE *in_fp;
  struct Framebuffer fbs[NUM_OUTPUT_BUFFERS] = {0};
  struct Screen screens[1] = {
      {.buffers = fbs, .num_buffers = NUM_OUTPUT_BUFFERS, 0}};
  // struct Decoder decoder = {0};
  struct perf_clock start, end = {0};
  struct v4l2_plane planes[FMT_NUM_PLANES] = {0};
  struct v4l2_buffer buffer = {.m.planes = planes, 0};
  struct epoll_event ev = {0};
  struct epoll_event pevents[MAX_EVENTS] = {0};

  struct buffer_mp in_buffers[NUM_INPUT_BUFFERS] = {0};
  struct buffer_mp out_buffers[screens[0].num_buffers];
  bzero(&out_buffers, sizeof(out_buffers));
  struct buffer_mp free_buffers_data[NUM_INPUT_BUFFERS] = {0};
  struct Queue free_buffers = {.length = NUM_INPUT_BUFFERS + 1,
                               .data = (void **)&free_buffers_data};

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

  // We don't queue buffer 0 because we are pretending thats the one on screen
  // and you can't write to the buffer on screen or you get tearing
  for (int idx = 1; idx < sizeof(out_buffers) / sizeof(out_buffers[0]); idx++) {
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

    printf("Input Buffer %d start: %p, length: %d, size: %d\n", idx,
           in_buffers[idx].start[0], in_buffers[idx].length[0],
           in_buffers[idx].size[0]);

    bzero(&buffer, sizeof(buffer));
    bzero(&planes, sizeof(planes));
    buffer.m.planes = planes;
    init_v4l2_buffer(&buffer, idx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                     V4L2_MEMORY_MMAP, FMT_NUM_PLANES, NULL, NULL);

    printf("Buffer: idx: %d, type: %d, bytesused: %d, flags: %d, field: "
           "%d, memory: %d, len: %d, m.planes: %p\n",
           buffer.index, buffer.type, buffer.bytesused, buffer.flags,
           buffer.field, buffer.memory, buffer.length, buffer.m.planes);
    for (int p = 0; p < FMT_NUM_PLANES; p++) {
      printf(
          "Plane: idx: %d, bytesused: %d, length: %d, mem_offset: %d, fd: %d\n",
          p, planes[p].bytesused, planes[p].length, planes[p].m.mem_offset,
          planes[p].m.fd);
    }

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
    int err = queue_write(&free_buffers, in_buffers + idx);
    if (err) {
      fprintf(stderr, "Unable to write to free buffer queue\n");
      return err;
    }
    printf("Wrote buffer %d to queue.\n", idx);
  }

  if (start_stream(drm_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0) {
    fprintf(stderr, "Failed to start output stream\n");
    return -1;
  }
  if (start_stream(drm_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
    fprintf(stderr, "Failed to start capture stream\n");
    return -1;
  }

  /**
   * Listen for UDP packets
   */
  // udp_socket_fd = init_socket(7255);

  // ev.events = EPOLLIN;
  // ev.data.fd = udp_socket_fd;

  // TODO: Check for errors
  int poll_fd = create_epoller(drm_fd);

  // if (epoll_ctl(poll_fd, EPOLL_CTL_ADD, udp_socket_fd, &ev) != 0) {
  //   fprintf(stderr, "Unable to add fd to epoll, status = %s\n",
  //           strerror(errno));
  //   return -1;
  // }

  for (;;) {
    // bzero(&buffer, sizeof(buffer));
    // bzero(&planes, sizeof(planes));
    // buffer.m.planes = planes;

    int status = epoll_events(poll_fd, pevents);
    // printf("Poll result: %d\n", status);

    if (status < 0) {
      fprintf(stderr, "Failed to poll events!\n");
      break;
    }

    for (int i = 0; i < status; i++) {
      struct epoll_event event = pevents[i];
      epoll_data_t data = pevents[i].data;
      if (pevents[i].data.fd == drm_fd) {
        handle_drm_event_file(&pevents[i], &screens[0], in_buffers, out_buffers,
                              dri_fd, in_fp, &in_offset);
        // handle_drm_event_udp(&pevents[i], &screens[0], in_buffers,
        // out_buffers,
        //                      dri_fd, &free_buffers);
      } else if (pevents[i].data.fd == udp_socket_fd) {
        // handle_udp_event returns 1 if a frame was writen
        int frame_written = handle_udp_event(&pevents[i], &free_buffers);
        if (frame_written < 0) {
          fprintf(stderr, "Unable to handle UDP event\n");
          return -1;
        } else if (frame_written > 0) {
          // end = get_perf();
          // display_diff_perf(&start, &end, "FPS");
          // start = get_perf();

          // Read the full buffer from the queue
          struct buffer_mp *filled_buffer = queue_read(&free_buffers);
          if (filled_buffer == NULL) {
            fprintf(stderr, "Unable to read from free buffers queue\n");
            return -1;
          }

          // Queue the buffer into the V4L2 device
          bzero(&buffer, sizeof(buffer));
          bzero(&planes, sizeof(planes));
          buffer.m.planes = planes;
          init_v4l2_buffer(&buffer, filled_buffer - in_buffers,
                           V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP,
                           FMT_NUM_PLANES, NULL, NULL);

          if (queue_buffer(drm_fd, &buffer) < 0) {
            fprintf(stderr, "Failed to queue buffer\n");
            return -1;
          }
        }
      }
    }
    // usleep(100000);
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
