// https://gist.github.com/ryankurte/61f95dc71133561ed055ff62b33585f8
// A simple fifo queue (or ring buffer) in c.
// This implementation \should be\ "thread safe" for single producer/consumer
// with atomic writes of size_t. This is because the head and tail "pointers"
// are only written by the producer and consumer respectively. Demonstrated with
// void pointers and no memory management. Note that empty is head==tail, thus
// only QUEUE_SIZE-1 entries may be used.

#include "safe_queue.h"
#include <stdio.h>

void *queue_read(queue_t *queue) {
  if (queue->tail == queue->head) {
    return NULL;
  }
  void *handle = queue->data[queue->tail];
  queue->data[queue->tail] = NULL;
  queue->tail = (queue->tail + 1) % queue->length;
  return handle;
}

int queue_write(queue_t *queue, void *handle) {
  if (((queue->head + 1) % queue->length) == queue->tail) {
    return -1;
  }
  queue->data[queue->head] = handle;
  queue->head = (queue->head + 1) % queue->length;
  return 0;
}

void *queue_peek(queue_t *queue) {
  if (queue->tail == queue->head) {
    return NULL;
  }
  void *handle = queue->data[queue->tail];

  return handle;
}

void *queue_peek_next(queue_t *queue) {
  if (((queue->tail + 1) % queue->length) == queue->head) {
    return NULL;
  }
  void *handle = queue->data[(queue->tail + 1) % queue->length];

  return handle;
}

int queue_size(queue_t *queue) {
  if (queue->head > queue->tail) {
    return queue->head - queue->tail;
  } else {
    return queue->length - queue->tail + queue->head;
  }
  printf("Head: %zu, Tail: %zu\n", queue->head, queue->tail);
}