#ifndef FILE_UTILS_H
#define FILE_UTILS_H

#include <sys/types.h>
#include <stddef.h>

/* File I/O & directory management */
int read_file(const char *path, char *buf, size_t max_len);
int write_file(const char *path, const char *content, mode_t mode);
int append_file(const char *path, const char *content);
int mkdir_p(const char *path, mode_t mode);

#endif /* FILE_UTILS_H */
