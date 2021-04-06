#pragma once

#include <stdlib.h>

typedef struct Queue {
    size_t head;
    size_t tail;
    size_t length;
    void** data;
} queue_t;

void* queue_read(queue_t *queue);
int queue_write(queue_t *queue, void* handle);
void* queue_peek(queue_t *queue);
void* queue_peek_next(queue_t *queue);
int queue_size(queue_t *queue);
