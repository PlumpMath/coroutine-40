#include "coroutine.h"

#include "queue.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif
#include <ucontext.h>

#include <pthread.h>

#include <unistd.h>

struct context {
    ucontext_t uc;
};

struct scheduler {
    uint32_t id;
    pthread_t tid;
    stack_t stack;
    struct queue *self_queue;
    struct queue *lock_queue;
    struct queue *empty_queue;
    size_t lock_queue_n;

    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_signal;
    struct context ctx;
    char _padding[256];
};

enum coroutine_status {
    STATUS_QUEUE,
    STATUS_RUNNING,
    STATUS_EXITING,
    STATUS_ASYNC,
};

struct coroutine {
    bool inited;
    enum coroutine_status status;
    const char *name;
    void (*free_name)(void *);
    struct context ctx;
    struct scheduler *sched;
    coroutine_function func;
    void *result;
    uint8_t *stack_lowest;
    uint8_t *stack_base;
    size_t stack_size;
};

__thread struct scheduler *SCHEDULER;
__thread struct coroutine *COROUTINE;

struct scheduler *
_scheduler() {
    return SCHEDULER;
}

static void
_set_scheduler(struct scheduler *s) {
    SCHEDULER = s;
}

static struct coroutine *
_running_coroutine() {
    return COROUTINE;
}

static void
_set_running_coroutine(struct coroutine *co) {
    COROUTINE = co;
}

static void
_coroutine_main(uint32_t lo, uint32_t hi) {
    uint64_t value = (uint64_t)lo | ((uint64_t)hi << 32);
    struct coroutine *co = (struct coroutine *)(uintptr_t)value;
    assert(co == _running_coroutine());
    coroutine_function func = co->func;
    void *argument = co->result;
    func(argument);
    // FIXME return from here is not working in FreeBSD 10.0.
#if 0
    co->result = NULL;
    co->status = STATUS_EXITING;
#else
    coroutine_exit();
#endif
}

static void
_make_context(struct coroutine *co, struct context *thread, stack_t *stack) {
    struct context *ctx = &co->ctx;
    // XXX ucontext API was deprecated in Mac OS X.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    int err = getcontext(&ctx->uc);
    assert(err == 0);

    ctx->uc.uc_link = &thread->uc;
    ctx->uc.uc_stack = *stack;

    uint64_t value = (uint64_t)(uintptr_t)co;
    makecontext(&ctx->uc, (void(*)())_coroutine_main, 2, (uint32_t)value, (uint32_t)(value>>32));
#pragma GCC diagnostic pop
}

static void
_switch_context(struct context *from, struct context *to) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    swapcontext(&from->uc, &to->uc);
#pragma GCC diagnostic pop
}

static struct scheduler * _select_scheduler();
static void _push_coroutine(struct scheduler *s, struct coroutine *co);

inline static void
_queue(struct scheduler *s, struct coroutine *co) {
    queue_push(s->self_queue, co);
}

static void *
_yield(struct coroutine *co) {
    assert(_scheduler() == co->sched);
    co->stack_lowest = (uint8_t*)&co;
    co->result = NULL;
    _switch_context(&co->ctx, &co->sched->ctx);
    return co->result;
}

struct coroutine *
coroutine_new(coroutine_function func) {
    struct coroutine *co = calloc(1, sizeof(struct coroutine));
    struct scheduler *s = _select_scheduler();
    co->name = "anonymous";
    co->sched = s;
    co->func = func;
    return co;
}

static void
coroutine_delete(struct coroutine *co) {
    if (co->free_name) co->free_name((void*)co->name);
    free(co->stack_base);
    free(co);
}

const char *
coroutine_name(struct coroutine *co) {
    return co->name;
}

void
coroutine_set_name(struct coroutine *co, const char *name, void (*free_name)(void *)) {
    co->name = name;
    co->free_name = free_name;
}

void
coroutine_exit() {
    struct coroutine *co = _running_coroutine();
    co->status = STATUS_EXITING;
    _yield(co);
}

void *
coroutine_yield() {
    struct coroutine *co = _running_coroutine();
    co->status = STATUS_QUEUE;
    return _yield(co);
}

void
coroutine_resume(struct coroutine *co, void *result) {
    co->result = result;
    co->status = STATUS_QUEUE;
    _push_coroutine(co->sched, co);
}

uint64_t
coroutine_sleep(uint64_t msecs) {
    struct coroutine *co = _running_coroutine();
    co->status = STATUS_ASYNC;
    if (msecs == 0) {
        co->status = STATUS_QUEUE;
        _queue(co->sched, co);
    }
    // FIXME insert co to sleeping queue, wait for wakeup.
    return (uint64_t)(uintptr_t)_yield(co);
}

void
coroutine_wakeup(struct coroutine *co, uint64_t remains) {
    assert(co->status == STATUS_ASYNC);
    coroutine_resume(co, (void*)(uintptr_t)remains);
}

static uint8_t *
_stack_addr(struct scheduler *s, size_t size) {
    return ((uint8_t*)s->stack.ss_sp + s->stack.ss_size) - size;
}

static size_t
_stack_size(struct scheduler *s, uint8_t *addr) {
    return (size_t)(((uint8_t*)s->stack.ss_sp + s->stack.ss_size) - addr) + 256;
}

static void
_save_stack(struct coroutine *co) {
    size_t size = _stack_size(co->sched, co->stack_lowest);
    assert(size <= co->sched->stack.ss_size);
    co->stack_base = realloc(co->stack_base, size);
    co->stack_size = size;
    memcpy(co->stack_base, _stack_addr(co->sched, size), size);
}

static void
_copy_stack(struct coroutine *co) {
    memcpy(_stack_addr(co->sched, co->stack_size), co->stack_base, co->stack_size);
}

inline static void
_resume(struct coroutine *co) {
    struct scheduler *s = co->sched;
    assert(s == _scheduler());
    if (co->inited) {
        _copy_stack(co);
    } else {
        // init in the same thread as coroutine context.
        co->inited = true;
        _make_context(co, &s->ctx, &s->stack);
    }
    co->status = STATUS_RUNNING;
    _switch_context(&s->ctx, &co->ctx);
}

static void
_run(struct coroutine *co) {
    _set_running_coroutine(co);
    _resume(co);
    _set_running_coroutine(NULL);
    if (co->status == STATUS_EXITING) {
        coroutine_delete(co);
        return;
    }
    _save_stack(co);
}

static void
_push_coroutine(struct scheduler *s, struct coroutine *co) {
    struct scheduler *self = _scheduler();
    if (s == self) {
        queue_push(s->self_queue, co);
    } else {
        pthread_mutex_lock(&s->queue_mutex);
        s->lock_queue_n += 1;
        queue_push(s->lock_queue, co);
        pthread_mutex_unlock(&s->queue_mutex);
        pthread_cond_signal(&s->queue_signal);
    }
}

static struct coroutine *
_next_coroutine(struct scheduler *s) {
    if (queue_empty(s->self_queue)) {
        pthread_mutex_lock(&s->queue_mutex);
        while (queue_empty(s->lock_queue)) {
            pthread_cond_wait(&s->queue_signal, &s->queue_mutex);
        }
        assert(queue_empty(s->self_queue));
        struct queue *q = s->lock_queue;
        s->lock_queue = s->self_queue;
        s->lock_queue_n = 0;
        pthread_mutex_unlock(&s->queue_mutex);
        s->self_queue = q;
        return queue_pop(q);
    }
    if (s->lock_queue_n != 0) {
        pthread_mutex_lock(&s->queue_mutex);
        struct queue *q = s->lock_queue;
        s->lock_queue = s->empty_queue;
        s->lock_queue_n = 0;
        pthread_mutex_unlock(&s->queue_mutex);
        queue_concat(s->self_queue, q);
        s->empty_queue = q;
    }
    return queue_pop(s->self_queue);
}

static void *
_schedule(void *arg) {
    struct scheduler *s = arg;
    _set_scheduler(s);
    for (;;) {
        assert(_running_coroutine() == NULL);
        struct coroutine *co = _next_coroutine(s);
        assert(co->sched == s);
        _run(co);
    }
    return NULL;
}

enum { kStackSize = 1024*1024*10 };

static struct scheduler *
_new_scheduler(uint32_t id) {
    struct scheduler *s = calloc(1, sizeof(*s));
    s->id = id;
    s->self_queue = queue_new(255);
    s->lock_queue = queue_new(255);
    s->empty_queue = queue_new(255);
    s->stack.ss_sp = valloc(kStackSize);
    s->stack.ss_size = kStackSize;
    pthread_mutex_init(&s->queue_mutex, NULL);
    pthread_cond_init(&s->queue_signal, NULL);
    return s;
}

struct scheduler **schedulers;
int nscheduler;

static struct scheduler *
_select_scheduler() {
    int i = rand()%nscheduler;
    return schedulers[i];
}

int
schedule_start(int n) {
    if (n <= 0) {
        return EINVAL;
    }
    nscheduler = n;
    struct scheduler **ss = schedulers = calloc(n, sizeof(struct scheduler *));
    for (int i=0; i<n; ++i) {
        ss[i] = _new_scheduler((uint32_t)(i+1));
    }
    for (int i=0; i<n; ++i) {
        pthread_create(&ss[i]->tid, NULL, _schedule, ss[i]);
    }
    return 0;
}
