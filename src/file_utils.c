#include "file_utils.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

/* Low-level POSIX I/O utilities using O_CLOEXEC */

int read_file(const char *path, char *buf, size_t max_len)
{
    if (!path || !buf || max_len == 0)
        return -1;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    ssize_t r = read(fd, buf, max_len - 1);
    close(fd);

    if (r <= 0)
        return -1;

    buf[r] = '\0';
    return (int)r;
}

int write_file(const char *path, const char *content, mode_t mode)
{
    if (!path || !content)
        return -1;

    mode_t open_mode = (mode != 0) ? mode : 0644;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, open_mode);
    if (fd < 0)
        return -1;

    size_t len = strlen(content);
    ssize_t written = 0;

    while ((size_t)written < len)
    {
        ssize_t res = write(fd, content + written, len - written);
        if (res < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        written += res;
    }

    close(fd);
    return 0;
}

int append_file(const char *path, const char *content)
{
    if (!path || !content)
        return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;

    size_t len = strlen(content);
    ssize_t written = 0;

    while ((size_t)written < len)
    {
        ssize_t res = write(fd, content + written, len - written);
        if (res < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        written += res;
    }

    close(fd);
    return 0;
}

int mkdir_p(const char *path, mode_t mode)
{
    if (!path || *path == '\0')
        return -1;

    char tmp[PATH_MAX];
    int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len < 0 || (size_t)len >= sizeof(tmp))
        return -1;

    /* Strip trailing slashes */
    while (len > 1 && tmp[len - 1] == '/')
    {
        tmp[len - 1] = '\0';
        len--;
    }

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;

    return 0;
}
