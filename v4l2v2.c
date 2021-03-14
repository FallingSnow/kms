
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