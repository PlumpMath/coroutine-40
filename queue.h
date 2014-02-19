#ifndef __CO_QUEUE_H_
#define __CO_QUEUE_H_

#include <stddef.h>
#include <stdbool.h>

struct queue;

struct queue *queue_new(size_t hint);
void queue_delete(struct queue *q);

void *queue_pop(struct queue *);
void queue_push(struct queue *, void *v);
bool queue_empty(struct queue *);
void queue_concat(struct queue *, struct queue *);

#endif
