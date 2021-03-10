#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEV_PATH "/dev/video10"
#define V4L2_BUFFER_TYPE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define FMT_NUM_PLANES 1

struct buffer {
  void *ptr;
  int length;
};

struct buffer_mp {
  void *start[FMT_NUM_PLANES];
  unsigned int length[FMT_NUM_PLANES];
};

struct Decoder {
  int deviceFd;
  struct buffer buffers[2];
  int num_buffers;
  struct buffer_mp *buffers_mp;
  struct buffer_mp *buffers_mp_out;
  unsigned int n_buffers;
  unsigned int n_buffers_out;
};

int init_decoder(struct Decoder *decoder);
int decode(struct Decoder *decoder, struct v4l2_buffer *buffer);
int get_buffers(struct Decoder *decoder);
int start_capturing(struct Decoder *decoder);
