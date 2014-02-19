#include "queue.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct queue {
    void **buf;
    size_t len;
    size_t cap;
    size_t off;
    size_t end;
};

struct queue *
queue_new(size_t hint) {
    struct queue *q = calloc(1, sizeof(struct queue));
    if (hint != 0) {
        size_t cap = 0;
        do {
            cap = 2*cap + 1;
        } while (hint > cap);
        q->cap = cap;
        q->buf = malloc(cap*sizeof(void*));
    }
    return q;
}

void
queue_delete(struct queue *q) {
    free(q->buf);
    free(q);
}

bool
queue_empty(struct queue *q) {
    return q->len == 0;
}

void
queue_concat(struct queue *q, struct queue *o) {
    void *data;
    while ((data = queue_pop(o)) != NULL) {
        queue_push(q, data);
    }
}

void *
queue_pop(struct queue *q) {
    if (queue_empty(q)) {
        return NULL;
    }
    size_t i = q->off == q->cap ? 0 : q->off;
    q->off = i+1;
    q->len--;
    return q->buf[i];
}

void
queue_push(struct queue *q, void *v) {
    size_t cap = q->cap;
    if (cap == q->len) {
        size_t newcap = 2*cap + 1;
        q->buf = realloc(q->buf, newcap*sizeof(void*));
        if (q->len != 0 && q->off == q->end) {
            size_t off = q->off;
            if (off <= cap/2) {
                memcpy(q->buf+cap, q->buf, off*sizeof(void*));
                q->end += cap;
            } else {
                size_t n = cap-off;
                size_t newoff = newcap - n;
                memcpy(q->buf+newoff, q->buf+off, n*sizeof(void*));
                q->off = newoff;
            }
        }
        q->cap = cap = newcap;
    }
    size_t i = q->end == cap ? 0 : q->end;
    q->buf[i] = v;
    q->end = i+1;
    q->len++;
}
