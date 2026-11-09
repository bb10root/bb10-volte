/* Run with `make test`. Stop signals must reach only the main thread, whose sleep()
 * they interrupt, so start_worker() blocks them in workers and leaves main as it was. */
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>

/* The real header needs QNX's slog2. */
#define LOG_UTILS_H
#define LOG_VERBOSE(...) ((void)0)

#include "sys_utils.c"

int read_file(const char *path, char *buf, size_t max_len)
{
    (void)path, (void)buf, (void)max_len;
    return -1;
}

static int blocked(int sig)
{
    sigset_t cur;
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    return sigismember(&cur, sig);
}

static int g_worker_ok = 0;

static void *worker(void *arg)
{
    (void)arg;
    g_worker_ok = blocked(SIGTERM) && blocked(SIGINT) && !blocked(SIGUSR1);
    return NULL;
}

int main(void)
{
    pthread_t t;

    assert(start_worker(&t, worker) == 0);
    pthread_join(t, NULL);

    assert(g_worker_ok);
    assert(!blocked(SIGTERM) && !blocked(SIGINT));
    printf("test_stop_signal: ok\n");
    return 0;
}
