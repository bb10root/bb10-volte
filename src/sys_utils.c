#include "sys_utils.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <signal.h>

char g_init_path[512] = {0};
char g_data_path[512] = {0};

void init_addon_paths(void)
{
    char exe_path[512];

    if (read_file("/proc/self/exefile", exe_path, sizeof(exe_path)) > 0)
    {
        exe_path[strcspn(exe_path, "\r\n")] = '\0';

        char *init_dir = dirname(exe_path);
        snprintf(g_init_path, sizeof(g_init_path), "%s", init_dir);

        char init_dir_copy[512];
        snprintf(init_dir_copy, sizeof(init_dir_copy), "%s", init_dir);
        char *base_dir = dirname(init_dir_copy);
        snprintf(g_data_path, sizeof(g_data_path), "%s/data", base_dir);
    }
    else
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
        {
            snprintf(g_init_path, sizeof(g_init_path), "%s", cwd);
            char *parent = dirname(cwd);
            snprintf(g_data_path, sizeof(g_data_path), "%s/data", parent);
        }
        else
        {
            snprintf(g_init_path, sizeof(g_init_path), "./init");
            snprintf(g_data_path, sizeof(g_data_path), "./data");
        }
    }
}

/* Workers block SIGTERM, SIGINT and SIGHUP, so a stop or restart signal always reaches
 * the main thread and interrupts its sleep at once. */
int start_worker(pthread_t *thread, void *(*fn)(void *))
{
    sigset_t stop, old;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGHUP);

    pthread_sigmask(SIG_BLOCK, &stop, &old);
    int rc = pthread_create(thread, NULL, fn, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
}
