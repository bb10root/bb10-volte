#ifndef SYS_UTILS_H
#define SYS_UTILS_H

#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#include "log_utils.h"
#include "file_utils.h"
#include "proc_utils.h"

#define RUN_PATH "/var/tmp/bb10-volte"

extern char g_init_path[512];
extern char g_data_path[512];

/* General utility functions */
void init_addon_paths(void);
int start_worker(pthread_t *thread, void *(*fn)(void *));

#endif /* SYS_UTILS_H */
