#include "coroutine.h"

#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct arg {
    int n;
};

int n_started = 20000;
int n_stopped = 0;

void
test(struct arg *arg) {
    for (int i = 0; i<arg->n; ++i) {
        coroutine_sleep(0);
    }
    __sync_add_and_fetch(&n_stopped, 1);
}

int
main(int argc, const char *argv[]) {
    int nthread = 8;
    if (argc > 1) {
        nthread = atoi(argv[1]);
    }
    printf("n scheduler: %d\n", nthread);
    schedule_start(nthread);

    char namebuf[1024];
    struct arg arg = { 100 };
    for (int i=0; i<n_started; ++i) {
        snprintf(namebuf, sizeof namebuf, "test %d", i);
        struct coroutine *co = coroutine_new((coroutine_function)test);
        coroutine_set_name(co, strdup(namebuf), free);
        coroutine_resume(co, &arg);
    }
    while (n_stopped != n_started) {
        usleep(500);
    }
}
