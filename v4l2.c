#define _GNU_SOURCE
#include <stdio.h>

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

FILE *in_fp;
unsigned long f_offset = 0;

static int xioctl(int fh, int request, void *arg) {
  int r;

  do {
    r = ioctl(fh, request, arg);
  } while (-1 == r && EINTR == errno);

  return r;
}

static void process_image(const void *ptr, int size) {
  printf("Writing %d bytes to output.\n", size);
  // if (!out_fp)
  //     out_fp = fopen(out_filename, "wb");
  // if (out_fp)
  //     fwrite(ptr, size, 1, out_fp);

  // fflush(stderr);
  // fprintf(stderr, ".");
}

static void process_image_mp(void *ptr[], unsigned int size[]) {
  unsigned int p;

  for (p = 0; p < FMT_NUM_PLANES; ++p)
    process_image(ptr[p], size[p]);
}

static void supply_input(void *buf, unsigned int buf_len,
                         unsigned int *bytesused) {
  unsigned char *buf_char = (unsigned char *)buf;
  if (in_fp) {
    *bytesused = fread(buf, 1, buf_len, in_fp);
    if (*bytesused != buf_len)
      fprintf(stderr, "Short read %u instead of %u\n", *bytesused, buf_len);
    else
      fprintf(stderr, "Read %u bytes. First 4 bytes %02x %02x %02x %02x\n",
              *bytesused, buf_char[0], buf_char[1], buf_char[2], buf_char[3]);
  }
}

static int supply_input_by_au(void *buf, size_t buf_len,
                              unsigned int *bytesused) {
  unsigned char *buf_char = (unsigned char *)buf,
                aud[4] = {0, 0, 0, 1}; // Start sequence for a h264 AUD,
                                       // denotes beginning of a single frame
  size_t au_length;
  void *b_offset;

  if (-1 == fseek(in_fp, f_offset, SEEK_SET)) {
    fprintf(stderr, "Failed to seek in input file\n");
    return -1;
  }
  *bytesused = fread(buf, 1, buf_len, in_fp);
  f_offset += *bytesused;
  if (*bytesused != buf_len) {
    printf("EOF of input reached\n");
  }

  printf("Searching for AUD in %p. [Buf: %p, read: %u]\n", in_fp, buf,
         *bytesused);
  printf("Used %u bytes. First 8 bytes %02x %02x %02x %02x\n", *bytesused,
         buf_char[0], buf_char[1], buf_char[2], buf_char[3]);

  // TODO: Upgrade memmem to https://github.com/mischasan/aho-corasick
  b_offset = memmem(buf + 1, *bytesused, aud, sizeof(aud) / sizeof(aud[0]));
  if (b_offset == NULL) {
    fprintf(stderr, "Next AUD not found!\n");
    return 0;
  }

  au_length = b_offset - buf;
  f_offset -= *bytesused - au_length;

  // Clear AUD
  printf("AUD found at %lu, Setting %lu bytes to 0\n", f_offset, buf_len - au_length);
  memset(b_offset, 0, *bytesused - au_length);

  return 0;
}

static void supply_input_mp(void *buf[], unsigned int buf_len[],
                            unsigned int *bytesused) {
  unsigned int p;
  unsigned int tot_bytes = 0;

  for (p = 0; p < FMT_NUM_PLANES; ++p) {
    unsigned int bytes;
    printf("supply_input_mp p: %d, buffersize: %u\n", p, buf_len[p]);
    supply_input_by_au(buf[p], buf_len[p], &bytes);
    // supply_input(buf[p], buf_len[p], &bytes);
    tot_bytes += bytes;
  }

  *bytesused = tot_bytes;
}

static int read_frame_mmap_mp(int deviceFd, enum v4l2_buf_type type,
                              struct buffer_mp *bufs, unsigned int n_buffers) {
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
    process_image_mp(bufs[buf.index].start, bufs[buf.index].length);
  else
    supply_input_mp(bufs[buf.index].start, bufs[buf.index].length,
                    &buf.bytesused);

  if (-1 == xioctl(deviceFd, VIDIOC_QBUF, &buf)) {
    fprintf(stderr, "VIDIOC_QBUF failed, %s\n", strerror(errno));
    return -1;
  }

  return 1;
}

static int start_capturing_mmap_mp(int deviceFd, enum v4l2_buf_type type,
                                   struct buffer_mp *bufs, size_t n_bufs) {
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
      supply_input_mp(bufs[i].start, bufs[i].length, &buf.bytesused);

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

  printf("Starting to capture on capture planes\n");
  err = start_capturing_mmap_mp(decoder->deviceFd,
                                V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                decoder->buffers_mp, decoder->n_buffers);
  if (err)
    return err;

  printf("Starting to capture on output planes\n");
  err = start_capturing_mmap_mp(
      decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      decoder->buffers_mp_out, decoder->n_buffers_out);
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

  req.count = 4;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;

  if (-1 == xioctl(deviceFd, VIDIOC_REQBUFS, &req)) {
    fprintf(stderr, "Mmap not supported, %s\n", strerror(errno));
    return -1;
  }

  if (req.count < 2) {
    fprintf(stderr, "Insufficient device memory\n");
    return -1;
  }

  bufs = calloc(req.count, sizeof(*bufs));

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
  format.fmt.pix_mp.field = V4L2_FIELD_NONE;

  if (-1 == xioctl(decoder->deviceFd, VIDIOC_S_FMT, &format)) {
    fprintf(stderr, "Failed VIDIOC_S_FMT, %s\n", strerror(errno));
    return -1;
  }

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

static void handle_event(struct Decoder *decoder) {
  struct v4l2_event ev;

  while (!ioctl(decoder->deviceFd, VIDIOC_DQEVENT, &ev)) {
    switch (ev.type) {
    case V4L2_EVENT_SOURCE_CHANGE:
      printf("Source changed\n");

      // stop_capture(V4L2_BUF_TYPE_VIDEO_CAPTURE);

      // unmap_buffers_mp(decoder->buffers_mp, decoder->n_buffers);

      // printf("Unmapped all buffers\n");

      // free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

      // init_mmap_mp(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &decoder->buffers_mp,
      // &decoder->n_buffers);

      // start_capturing_mmap_mp(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      // decoder->buffers_mp,
      //                         decoder->n_buffers);
      break;
    case V4L2_EVENT_EOS:
      fprintf(stderr, "EOS\n");
      break;
    }
  }
}

static void mainloop(struct Decoder *decoder) {
  unsigned int count;

  while (true) {
    for (;;) {
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
          continue;
        fprintf(stderr, "Select failed, %s\n", strerror(errno));
        return;
      }

      if (0 == r) {
        printf("select timeout\n");
        return;
      }

      if (rd_fds && FD_ISSET(decoder->deviceFd, rd_fds)) {
        printf("Reading\n");
        int read = read_frame_mmap_mp(decoder->deviceFd,
                                      V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                      decoder->buffers_mp, decoder->n_buffers);
        if (read > 0)
          break;
        else if (read < 0) {
          fprintf(stderr, "Writing failed\n");
          return;
        }
      }
      if (wr_fds && FD_ISSET(decoder->deviceFd, wr_fds)) {
        fprintf(stderr, "Writing\n");
        int read = read_frame_mmap_mp(
            decoder->deviceFd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            decoder->buffers_mp_out, decoder->n_buffers_out);
        if (read > 0)
          break;
        else if (read < 0) {
          fprintf(stderr, "Writing failed\n");
          return;
        }
      }
      if (ex_fds && FD_ISSET(decoder->deviceFd, ex_fds)) {
        fprintf(stderr, "Exception\n");
        handle_event(decoder);
      }
      /* EAGAIN - continue select loop. */
    }
  }
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

  in_fp = fopen("/home/alarm/jellyfish-15-mbps-hd-h264.h264", "rb");
  if (in_fp == NULL) {
    printf("Failed to open input video");
    return -1;
  }

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

  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  format.fmt.pix_mp.width = 1920;
  format.fmt.pix_mp.height = 1080;
  format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  format.fmt.pix_mp.field = V4L2_FIELD_ANY;

  if (xioctl(deviceFd, VIDIOC_S_FMT, &format) < 0) {
    fprintf(stderr, "Unable to set format, %s\n", strerror(errno));
    return -1;
  }

  // /* Buggy driver paranoia. */
  // unsigned int p;
  // for (p = 0; p < FMT_NUM_PLANES; ++p) {
  //   min = format.fmt.pix_mp.width * 2;
  //   if (format.fmt.pix_mp.plane_fmt[p].bytesperline > 0 &&
  //       format.fmt.pix_mp.plane_fmt[p].bytesperline < min)
  //     format.fmt.pix_mp.plane_fmt[p].bytesperline = min;
  //   min =
  //       format.fmt.pix_mp.plane_fmt[p].bytesperline *
  //       format.fmt.pix_mp.height;
  //   if (format.fmt.pix_mp.plane_fmt[p].sizeimage > 0 &&
  //       format.fmt.pix_mp.plane_fmt[p].sizeimage < min)
  //     format.fmt.pix_mp.plane_fmt[p].sizeimage = min;
  // }

  init_mmap_mp(deviceFd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
               &decoder->buffers_mp, &decoder->n_buffers);

  if (capability.capabilities &
      (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) {
    init_device_out(decoder);
  }

  return 0;
}

int decode(struct Decoder *decoder, struct v4l2_buffer *buffer) { return 0; }

//------------------------------------------------------------------------------
// Queue Buffer
//------------------------------------------------------------------------------

// https://modelingwithdata.org/arch/00000022.htm
// These are the parameters you can pass the queue_buffer
typedef struct {
  struct Decoder *decoder;
  struct v4l2_buffer *buffer;
  enum v4l2_buf_type type;
} queue_buffer_args;

// The actual qeue_buffer function
int queue_buffer_base(struct Decoder *decoder, struct v4l2_buffer *buffer) {

  // Queues buffer for decoding.
  if (-1 == xioctl(decoder->deviceFd, VIDIOC_QBUF, buffer)) {
    fprintf(stderr, "Unable to queue buffer, %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

// Setup default parameters
struct v4l2_buffer *_queue_buffer_base(queue_buffer_args args) {
  // If we weren't passed a buffer we need to create one
  bool created = false;
  if (!args.buffer) {
    args.buffer = calloc(1, sizeof(struct v4l2_buffer));
    args.buffer->type = args.type ? args.type : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    args.buffer->memory = V4L2_MEMORY_MMAP;
    created = true;
  }
  int result = queue_buffer_base(args.decoder, args.buffer);
  if (result < 0 && created) {
    free(args.buffer);
  }
  return args.buffer;
}

// Define the function that is called
#define queue_buffer(...) _queue_buffer_base((queue_buffer_args){__VA_ARGS__});

//------------------------------------------------------------------------------
// Dequeue Buffer
//------------------------------------------------------------------------------

typedef struct {
  struct Decoder *decoder;
  struct v4l2_buffer *buffer;
} dequeue_buffer_args;

int dequeue_buffer_base(struct Decoder *decoder, struct v4l2_buffer *buffer) {

  if (-1 == xioctl(decoder->deviceFd, VIDIOC_DQBUF, buffer)) {
    fprintf(stderr, "Unable to dequeue buffer, %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

// Setup default parameters
struct v4l2_buffer *_dequeue_buffer_base(queue_buffer_args args) {
  // If we weren't passed a buffer we need to create one
  bool created = false;
  if (!args.buffer) {
    args.buffer = calloc(1, sizeof(struct v4l2_buffer));
    args.buffer->type = args.type ? args.type : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    args.buffer->memory = V4L2_MEMORY_MMAP;
    created = true;
  }
  int result = dequeue_buffer_base(args.decoder, args.buffer);
  if (result < 0 && created) {
    free(args.buffer);
  }
  return args.buffer;
}

// Define the function that is called
#define dequeue_buffer(...)                                                    \
  _dequeue_buffer_base((queue_buffer_args){__VA_ARGS__});

//------------------------------------------------------------------------------
// Request Buffers from device
//------------------------------------------------------------------------------

int get_buffers(struct Decoder *decoder) {
  int count = 2;

  // Request some buffers on the device
  struct v4l2_requestbuffers req = {
      .count = count, .type = V4L2_BUFFER_TYPE, .memory = V4L2_MEMORY_MMAP, 0};

  // Make the request
  if (-1 == xioctl(decoder->deviceFd, VIDIOC_REQBUFS, &req)) {
    fprintf(stderr, "Could not request buffers, %s\n", strerror(errno));
    return -1;
  }

  // Check if we received less buffers than we asked for
  if (req.count < count) {
    fprintf(stderr, "Insufficient device buffer memory\n");
    return -1;
  }

  // For each buffer on the device, we are going to mmap it so we can access it
  // from userspace
  for (size_t idx = 0; idx < req.count; idx++) {
    struct v4l2_buffer v4l2_buffer = {0};
    struct v4l2_plane planes[2] = {0};

    v4l2_buffer.type = V4L2_BUFFER_TYPE;
    v4l2_buffer.memory = V4L2_MEMORY_MMAP;
    v4l2_buffer.index = idx;
    v4l2_buffer.m.planes = planes;
    v4l2_buffer.length = 2;

    if (-1 == xioctl(decoder->deviceFd, VIDIOC_QUERYBUF, &v4l2_buffer)) {
      fprintf(stderr, "Unable to query buffer %zu, %s\n", idx, strerror(errno));
      return -1;
    }

    decoder->buffers[idx].length = v4l2_buffer.length;
    decoder->buffers[idx].ptr =
        mmap(NULL, v4l2_buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
             decoder->deviceFd, v4l2_buffer.m.offset);

    if (decoder->buffers[idx].ptr == MAP_FAILED) {
      fprintf(stderr, "Could not mmap buffer %zu\n", idx);
      return -1;
    }
  }

  return 0;
}