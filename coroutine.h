#ifndef __CO_COROUTINE_H_
#define __CO_COROUTINE_H_

#include <stdint.h>

struct coroutine;

typedef void (*coroutine_function)(void *argument);

struct coroutine *coroutine_new(coroutine_function func);

// 1) Initial resume, pass 'value' as argument to coroutine function;
// 2) otherwise, 'value' is the return value to coroutine_yield().
void coroutine_resume(struct coroutine *co, void *value);
void *coroutine_yield();

void coroutine_exit();
uint64_t coroutine_sleep(uint64_t msecs);

int schedule_start(int n);

const char * coroutine_name(struct coroutine *co);
void coroutine_set_name(struct coroutine *co, const char *name, void (*free_name)(void *));

#endif
