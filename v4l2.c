#define _GNU_SOURCE
#include <stdio.h>

#include "util.h"
#include "v4l2.h"
#include <assert.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* STEPS
 * - Initilize decoder -- init_decoder(&decoder)
 * - Queue buffer -- queue_buffer(&decoder)
 * -
 */

#define DEBUG(...) getenv("DEBUG") != NULL &&printf(__VA_ARGS__)

unsigned long f_offset = 0;

static int xioctl(int fh, int request, void *arg) {
  int r;

  do {
    r = ioctl(fh, request, arg);
  } while (-1 == r && EINTR == errno);

  return r;
}

char *strstr_last(const char *str1, int len1, const char *str2, int len2) {
  char *strp;

  strp = (char *)(str1 + len1 - len2);
  while (strp != str1) {
    if (*strp == *str2) {
      if (strncmp(strp, str2, len2) == 0)
        return strp;
    }
    strp--;
  }
  return NULL;
}

// static void memcpy_nv12(__u8 *output, int size, const __u8 *ptr, __u32
// uvoffset) {
//   for (int i = 0; i < size; i += 1) {
//     float y = ptr[i];
//     float u = ptr[i + 1];
//     float v = ptr[i + 2];
//     output[i] = y + 1.13983 * v;
//     output[i + 1] = y - 0.39465 * u - 0.58060 * v;
//     output[i + 2] = y + 2.03211 * u;
//     output[i + 3] = 0xFF;
//     // output_idx += 4;
//   }
// }
static void copy_nv12(__u8 *output, int size, const __u8 *ptr) {
  struct perf_clock start, end;
  // printf("Writing %d bytes to output buffer!\n", size);
  // start = get_perf();
  memcpy(output, ptr, size - 30720);
  // end = get_perf();
  // display_diff_perf(&start, &end);
  // usleep(100000);
  // memcpy(output + offsets[0], ptr, size / 2);
  // memcpy(output + size / 2, ptr + size / 2, size / 4);
  // memcpy(output + size / 2 + size / 4, ptr + size / 2 + size / 4, size / 4);
}

static void process_image(void *output, const void *ptr, int size) {
  DEBUG("-- process_img(%p, %p, %d)\n", output, ptr, size);
  // printf("Writing %d bytes to output.\n", size);
  // memset(output, 0x00ffffff, size);
  // memcpy_nv12_to_rgba(output, size, ptr);
  // memcpy(output, ptr, size);
  copy_nv12(output, size, ptr);
  // *((uint32_t *)output+500) = ;
  // fwrite(output, size, 1, out_fp);
}

static void process_image_mp(void *ptr[], unsigned int size[], void *output) {
  unsigned int p;
  DEBUG("-- process_image_mp()\n");

  for (p = 0; p < FMT_NUM_PLANES; ++p)
    process_image(output, ptr[p], size[p]);
}

static void supply_input(void *buf, unsigned int buf_len,
                         unsigned int *bytesused, void *source) {
  DEBUG("-- supply_input(%p, %d, %d, %p)\n", buf, buf_len, *bytesused, source);
  FILE *in = (FILE *)source;
  unsigned char *buf_char = (unsigned char *)buf;
  if (in) {
    *bytesused = fread(buf, 1, buf_len, in);
    if (*bytesused != buf_len)
      fprintf(stderr, "Short read %u instead of %u\n", *bytesused, buf_len);
    else
      fprintf(stderr, "Read %u bytes. First 4 bytes %02x %02x %02x %02x\n",
              *bytesused, buf_char[0], buf_char[1], buf_char[2], buf_char[3]);
  }
}

static int supply_input_by_au(void *buf, size_t buf_len,
                              unsigned int *bytesused, void *source) {
  DEBUG("-- supply_input_by_au(%p, %zu, %d, %p)\n", buf, buf_len, *bytesused,
        source);
  FILE *in = (FILE *)source;
  unsigned char *buf_char = (unsigned char *)buf,
                aud[4] = {0, 0, 0, 1}; // Start sequence for a h264 AUD,
                                       // denotes beginning of a single frame
  size_t au_length;
  void *b_offset;

  if (-1 == fseek(in, f_offset, SEEK_SET)) {
    fprintf(stderr, "Failed to seek in input file\n");
    return -1;
  }
  // OPTIMIZE: We shouldn't reread, instead we should mask the data passed to
  // the buffer. Maybe use a buffered reader?
  *bytesused = fread(buf, 1, buf_len, in);
  if (*bytesused != buf_len) {
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
  b_offset = memmem(buf + 1, *bytesused, aud, sizeof(aud) / sizeof(aud[0]));
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
  memset(b_offset, 0, *bytesused - au_length);
  *bytesused = au_length;
  f_offset += au_length + 1;

  return 0;
}

static void supply_input_mp(void *buf[], unsigned int buf_len[],
                            unsigned int *bytesused, void *source) {
  unsigned int p;
  unsigned int tot_bytes = 0;
  DEBUG("-- supply_input_mp(%p, %p, %d, %p)\n", buf, buf_len, *bytesused,
        source);

  for (p = 0; p < FMT_NUM_PLANES; ++p) {
    unsigned int bytes;
    // printf("supply_input_mp p: %d, buffersize: %u\n", p, buf_len[p]);
    supply_input_by_au(buf[p], buf_len[p], &bytes, source);
    // supply_input(buf[p], buf_len[p], &bytes, source);
    tot_bytes += bytes;
  }

  *bytesused = tot_bytes;
}

static int read_frame_mmap_mp(int deviceFd, enum v4l2_buf_type type,
                              struct buffer_mp *bufs, unsigned int n_buffers,
                              void *target) {
  DEBUG("-- read_frame_mmap_mp(%d, %s, %p, %d, %p)\n", deviceFd,
        type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? "cap" : "out", bufs,
        n_buffers, target);
  struct v4l2_buffer buf = {0};
  struct v4l2_plane planes[FMT_NUM_PLANES] = {0};

  buf.type = type;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.length = FMT_NUM_PLANES;
  buf.m.planes = planes;

  if (-1 == xioctl(deviceFd, VIDIOC_DQBUF, &buf)) {
    switch (errno) {
    case EAGAIN:
      return 0;

    case EIO:
      /* Could ignore EIO, see spec. */

      /* fall through */

    default:
      fprintf(stderr, "VIDIOC_DQBUF failed, %s\n", strerror(errno));
      return -1;
    }
  }

  assert(buf.index < n_buffers);

  if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
    process_image_mp(bufs[buf.index].start, bufs[buf.index].length, target);
  else
    supply_input_mp(bufs[buf.index].start, bufs[buf.index].length,
                    &buf.bytesused, target);

  if (-1 == xioctl(deviceFd, VIDIOC_QBUF, &buf)) {
    fprintf(stderr, "VIDIOC_QBUF failed, %s\n", strerror(errno));
    return -1;
  }

  return 1;
}

static int start_capturing_mmap_mp(int deviceFd, enum v4l2_buf_type type,
                                   struct buffer_mp *bufs, size_t n_bufs,
                                   void *source) {
  DEBUG("-- start_capturing_mmap_mp(%d, %s, %p, %zu, %p)\n", deviceFd,
        type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? "cap" : "out", bufs,
        n_bufs, source);
  unsigned int i;

  for (i = 0; i < n_bufs; ++i) {
    printf("start_capturing_mmap_mp buffer: #%d, (%p) size: %u\n", i,
           bufs[i].start, *bufs[i].length);
    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[FMT_NUM_PLANES] = {0};

    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.length = FMT_NUM_PLANES;
    buf.m.planes = planes;

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
      supply_input_mp(bufs[i].start, bufs[i].length, &buf.bytesused, source);

    if (-1 == xioctl(deviceFd, VIDIOC_QBUF, &buf)) {
      fprintf(stderr, "Failed to queue buffer, %s\n", strerror(errno));
      return -1;
    }
  }

  if (-1 == xioctl(deviceFd, VIDIOC_STREAMON, &type)) {
    fprintf(stderr, "VIDIOC_STREAMON failed, %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

int start_capturing(struct Decoder *decoder) {
  unsigned int i;
  int err;
  DEBUG("-- start_capturing(output: %p, source: %p)\n", decoder->output,
        decoder->source);

  printf("Starting to capture on capture planes\n");
  err = start_capturing_mmap_mp(
      decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      decoder->buffers_mp, decoder->n_buffers, decoder->output);
  if (err)
    return err;

  printf("Starting to capture on output planes, file = %p\n", decoder->source);
  err = start_capturing_mmap_mp(
      decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      decoder->buffers_mp_out, decoder->n_buffers_out, decoder->source);
  if (err)
    return err;

  printf("Started capture\n");
  return 0;
}

static int init_mmap_mp(int deviceFd, enum v4l2_buf_type type,
                        struct buffer_mp **bufs_out, unsigned int *n_bufs) {
  struct v4l2_requestbuffers req = {0};
  struct buffer_mp *bufs = {0};
  unsigned int b;

  req.count = 1;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;

  if (-1 == xioctl(deviceFd, VIDIOC_REQBUFS, &req)) {
    fprintf(stderr, "Mmap not supported, %s\n", strerror(errno));
    return -1;
  }

  // if (req.count < 2) {
  //   fprintf(stderr, "Insufficient device memory\n");
  //   return -1;
  // }

  bufs = calloc(req.count, sizeof(*bufs));
  printf("Initilized %d v4l2 buffers\n", req.count);

  if (!bufs) {
    fprintf(stderr, "Out of memory\n");
    return -1;
  }

  for (b = 0; b < req.count; ++b) {
    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[FMT_NUM_PLANES] = {0};
    unsigned int p;

    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = b;
    /* length in struct v4l2_buffer in multi-planar API stores the size
     * of planes array. */
    buf.length = FMT_NUM_PLANES;
    buf.m.planes = planes;

    if (-1 == xioctl(deviceFd, VIDIOC_QUERYBUF, &buf)) {
      fprintf(stderr, "Failed VIDIOC_QUERYBUF, %s\n", strerror(errno));
      return -1;
    }

    fprintf(stderr, "Mapping %s buffer %u:\n",
            type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? "capture" : "output",
            b);
    for (p = 0; p < FMT_NUM_PLANES; ++p) {
      fprintf(stderr, "Mapping plane %u, len %u\n", p, buf.m.planes[p].length);

      bufs[b].length[p] = buf.m.planes[p].length;
      bufs[b].start[p] = mmap(NULL, buf.m.planes[p].length,
                              PROT_READ | PROT_WRITE, /* required */
                              MAP_SHARED,             /* recommended */
                              deviceFd, buf.m.planes[p].m.mem_offset);
      printf("Initilized buffer to length %u\n", buf.m.planes[p].length);

      if (MAP_FAILED == bufs[b].start[p]) {
        fprintf(stderr, "Failed to map buffer, %s\n", strerror(errno));
        return -1;
      }
    }
  }
  *n_bufs = b;
  *bufs_out = bufs;

  return 0;
}

static int init_device_out(struct Decoder *decoder) {
  struct v4l2_format format = {0};
  unsigned int min;

  format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

  if (-1 == xioctl(decoder->deviceFd, VIDIOC_G_FMT, &format)) {
    fprintf(stderr, "Failed VIDIOC_G_FMT, %s\n", strerror(errno));
    return -1;
  }

  format.fmt.pix_mp.width = 1920;
  format.fmt.pix_mp.height = 1080;
  format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
  format.fmt.pix_mp.field = V4L2_FIELD_ANY;

  /* Buggy driver paranoia. */
  unsigned int p;
  for (p = 0; p < FMT_NUM_PLANES; ++p) {
    min = format.fmt.pix_mp.width * 2;
    if (format.fmt.pix_mp.plane_fmt[p].bytesperline > 0 &&
        format.fmt.pix_mp.plane_fmt[p].bytesperline < min)
      format.fmt.pix_mp.plane_fmt[p].bytesperline = min;
    min =
        format.fmt.pix_mp.plane_fmt[p].bytesperline * format.fmt.pix_mp.height;
    if (format.fmt.pix_mp.plane_fmt[p].sizeimage > 0 &&
        format.fmt.pix_mp.plane_fmt[p].sizeimage < min)
      format.fmt.pix_mp.plane_fmt[p].sizeimage = min;
  }

  if (-1 == xioctl(decoder->deviceFd, VIDIOC_S_FMT, &format)) {
    fprintf(stderr, "Failed VIDIOC_S_FMT, %s\n", strerror(errno));
    return -1;
  }

  init_mmap_mp(decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
               &decoder->buffers_mp_out, &decoder->n_buffers_out);

  struct v4l2_event_subscription sub = {0};

  sub.type = V4L2_EVENT_EOS;
  if (-1 == ioctl(decoder->deviceFd, VIDIOC_SUBSCRIBE_EVENT, &sub)) {
    fprintf(stderr, "Failed VIDIOC_SUBSCRIBE_EVENT, %s\n", strerror(errno));
    return -1;
  }

  sub.type = V4L2_EVENT_SOURCE_CHANGE;
  if (-1 == ioctl(decoder->deviceFd, VIDIOC_SUBSCRIBE_EVENT, &sub)) {
    fprintf(stderr, "Failed VIDIOC_SUBSCRIBE_EVENT, %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

void stop_capture(struct Decoder *decoder, enum v4l2_buf_type type) {
  if (-1 == xioctl(decoder->deviceFd, VIDIOC_STREAMOFF, &type))
    fprintf(stderr, "Failed VIDIOC_STREAMOFF, %s\n", strerror(errno));
}

static void unmap_buffers_mp(struct buffer_mp *buf, unsigned int n) {
  unsigned int b, p;

  for (b = 0; b < n; b++)
    for (p = 0; p < FMT_NUM_PLANES; p++)
      if (-1 == munmap(buf[b].start[p], buf[b].length[p]))
        fprintf(stderr, "Failed to unmap buffers, %s\n", strerror(errno));
}

static void free_buffers_mmap(struct Decoder *decoder,
                              enum v4l2_buf_type type) {
  struct v4l2_requestbuffers req = {0};

  req.count = 0;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;

  if (-1 == xioctl(decoder->deviceFd, VIDIOC_REQBUFS, &req)) {
    fprintf(stderr, "Failed VIDIOC_REQBUFS, %s\n", strerror(errno));
  }
}

static int handle_event(struct Decoder *decoder) {
  struct v4l2_event ev = {0};

  do {
    if (ioctl(decoder->deviceFd, VIDIOC_DQEVENT, &ev) == -1) {
      fprintf(stderr, "Failed VIDIOC_REQBUFS, %s\n", strerror(errno));
      return -1;
    }
    switch (ev.type) {
    case V4L2_EVENT_SOURCE_CHANGE:
      printf("Source changed\n");

      stop_capture(decoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

      unmap_buffers_mp(decoder->buffers_mp, decoder->n_buffers);

      printf("Unmapped all buffers\n");

      free_buffers_mmap(decoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

      init_mmap_mp(decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                   &decoder->buffers_mp, &decoder->n_buffers);

      start_capturing_mmap_mp(
          decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
          decoder->buffers_mp, decoder->n_buffers, decoder->output);
      break;
    case V4L2_EVENT_EOS:
      fprintf(stderr, "EOS\n");
      return 1;
    }
  } while (ev.pending > 0);

  return 0;
}

int v4l2_write(struct Decoder *decoder) {
  int read = read_frame_mmap_mp(
      decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      decoder->buffers_mp_out, decoder->n_buffers_out, decoder->source);
  if (read > 0)
    return 0;
  else if (read < 0) {
    fprintf(stderr, "Writing failed\n");
    return -1;
  }
  return 0;
}
int v4l2_read(struct Decoder *decoder) {
  int read = read_frame_mmap_mp(
      decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      decoder->buffers_mp, decoder->n_buffers, decoder->output);
  if (read > 0)
    return 0;
  else if (read < 0) {
    fprintf(stderr, "Reading failed\n");
    return -1;
  }
  return 0;
}

int run(struct Decoder *decoder) {
  fd_set fds[3];
  fd_set *rd_fds = &fds[0]; /* for capture */
  fd_set *ex_fds = &fds[1]; /* for capture */
  fd_set *wr_fds = &fds[2]; /* for output */
  struct timeval tv;
  int r;

  if (rd_fds) {
    FD_ZERO(rd_fds);
    FD_SET(decoder->deviceFd, rd_fds);
  }

  if (ex_fds) {
    FD_ZERO(ex_fds);
    FD_SET(decoder->deviceFd, ex_fds);
  }

  if (wr_fds) {
    FD_ZERO(wr_fds);
    FD_SET(decoder->deviceFd, wr_fds);
  }

  /* Timeout. */
  tv.tv_sec = 10;
  tv.tv_usec = 0;

  r = select(decoder->deviceFd + 1, rd_fds, wr_fds, ex_fds, &tv);

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

  if (rd_fds && FD_ISSET(decoder->deviceFd, rd_fds)) {
    // printf("Reading\n");
    if (v4l2_read(decoder) == -1) {
      return -1;
    };
    return 1;
  }
  if (wr_fds && FD_ISSET(decoder->deviceFd, wr_fds)) {
    // printf("Writing\n");
    if (v4l2_write(decoder) == -1) {
      return -1;
    };
    return 2;
  }
  if (ex_fds && FD_ISSET(decoder->deviceFd, ex_fds)) {
    fprintf(stderr, "Exception\n");
    if (handle_event(decoder)) {
      return -1;
    }
    printf("Event handled\n");
  }
  return 0;
}

void mainloop(struct Decoder *decoder) {
  printf("Entered main loop\n");

  // while (true) {
  //   if (run(decoder) < 0) {
  //     return;
  //   }
  // }
}

//------------------------------------------------------------------------------
// Initialize Decoder
//------------------------------------------------------------------------------

int init_decoder(struct Decoder *decoder) {
  int deviceFd;
  struct v4l2_format format = {0};
  struct v4l2_capability capability = {0};
  struct v4l2_streamparm streamparm = {0};
  int err;
  unsigned int min;

  deviceFd = open(DEV_PATH, O_RDWR);
  if (deviceFd < 0) {
    printf("Error opening decode device, status = %d\n", deviceFd);
    return deviceFd;
  }
  decoder->deviceFd = deviceFd;

  if (xioctl(deviceFd, VIDIOC_QUERYCAP, &capability) < 0) {
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

  // Set output format
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  format.fmt.pix_mp.width = 1920;
  format.fmt.pix_mp.height = 1080;
  format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_RGB565;
  // format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
  // format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  format.fmt.pix_mp.field = V4L2_FIELD_NONE;

  /* Buggy driver paranoia. */
  unsigned int p;
  for (p = 0; p < FMT_NUM_PLANES; ++p) {
    min = format.fmt.pix_mp.width * 2;
    if (format.fmt.pix_mp.plane_fmt[p].bytesperline > 0 &&
        format.fmt.pix_mp.plane_fmt[p].bytesperline < min)
      format.fmt.pix_mp.plane_fmt[p].bytesperline = min;
    min =
        format.fmt.pix_mp.plane_fmt[p].bytesperline * format.fmt.pix_mp.height;
    if (format.fmt.pix_mp.plane_fmt[p].sizeimage > 0 &&
        format.fmt.pix_mp.plane_fmt[p].sizeimage < min)
      format.fmt.pix_mp.plane_fmt[p].sizeimage = min;
  }

  if (xioctl(deviceFd, VIDIOC_S_FMT, &format) < 0) {
    fprintf(stderr, "Unable to set format, %s\n", strerror(errno));
    return -1;
  }

  init_mmap_mp(deviceFd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
               &decoder->buffers_mp, &decoder->n_buffers);

  if (capability.capabilities &
      (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) {
    init_device_out(decoder);
  }

  return 0;
}