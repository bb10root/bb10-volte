#include "ims_routes.h"
#include "sys_utils.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* --- P-CSCF routes --- */

/* Each field runs to the next space and ends in a colon, which an IPv6 address can't be told apart from. */
static int take_address(char *field)
{
    size_t len = strlen(field);
    if (len < 2 || field[len - 1] != ':')
        return 0;
    field[--len] = '\0';
    return strspn(field, "0123456789abcdefABCDEF:.") == len;
}

/* ims_rrm's failed route command prints "add host <host>: gateway <gateway>: Permission denied".
   Mobily gives IPv6 P-CSCFs. */
int parse_route_failure(const char *line, char *host, char *gateway)
{
    int end = 0;
    if (sscanf(line, "add host %63s gateway %63s Permission denied%n", host, gateway, &end) != 2 || end == 0)
        return 0;
    return take_address(host) && take_address(gateway);
}

/* route reads an IPv6 address only after -inet6, and rejects it as "bad value" otherwise. */
static int run_route(const char *verb, const char *host, const char *gateway, const char *log_path)
{
    char *args[7];
    int n = 0;
    args[n++] = "route";
    args[n++] = (char *)verb;
    if (strchr(host, ':'))
        args[n++] = "-inet6";
    args[n++] = "-host";
    args[n++] = (char *)host;
    if (gateway)
        args[n++] = (char *)gateway;
    args[n] = NULL;
    return run_cmd_spawn(args, log_path);
}

/* ims_rrm can't add its P-CSCF routes without root, and reports each failure in
 * its log. Without them, replies to connections from the P-CSCF leave on the
 * default route, so the phone never receives a BYE sent over TCP. */
static char g_route_gateway[64] = "";

static int address_is_local(const char *address)
{
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
    struct sockaddr *sa;
    socklen_t len;
    memset(&v4, 0, sizeof(v4));
    memset(&v6, 0, sizeof(v6));

    if (inet_pton(AF_INET, address, &v4.sin_addr) == 1)
    {
        v4.sin_family = AF_INET;
        sa = (struct sockaddr *)&v4;
        len = sizeof(v4);
    }
    else if (inet_pton(AF_INET6, address, &v6.sin6_addr) == 1)
    {
        v6.sin6_family = AF_INET6;
        sa = (struct sockaddr *)&v6;
        len = sizeof(v6);
    }
    else
        return 1;

    int fd = socket(sa->sa_family, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;

    int gone = bind(fd, sa, len) != 0 && errno == EADDRNOTAVAIL;
    close(fd);
    return !gone;
}

/* These static routes outlive an IP change, such as airplane mode. ims_rrm reports its
 * routes just before it registers, too late for add_missing_ims_routes() to save that
 * REGISTER, and it doesn't retry the send. So clear them once their address is gone.
 * Only then: ims_rrm may not report them again while its address is unchanged. */
void delete_stale_ims_routes(void)
{
    char path[512];
    char route_log[512];
    char line[256];

    if (g_route_gateway[0] == '\0' || address_is_local(g_route_gateway))
        return;
    g_route_gateway[0] = '\0';

    snprintf(path, sizeof(path), "%s/ims-start.log", RUN_PATH);
    snprintf(route_log, sizeof(route_log), "%s/route.log", RUN_PATH);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    while (fgets(line, sizeof(line), f))
    {
        char host[64];
        char gateway[64];

        if (!parse_route_failure(line, host, gateway))
            continue;

        if (run_route("delete", host, NULL, route_log) == 0)
            log_msg("[BBE] IMS_ROUTE_CLEARED: %s", host);
    }

    fclose(f);
}

void add_missing_ims_routes(void)
{
    static long offset = 0;
    char path[512];
    char line[256];

    delete_stale_ims_routes();

    snprintf(path, sizeof(path), "%s/ims-start.log", RUN_PATH);
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    /* start_ims() truncates the log. */
    fseek(f, 0, SEEK_END);
    if (ftell(f) < offset)
        offset = 0;
    fseek(f, offset, SEEK_SET);

    while (fgets(line, sizeof(line), f))
    {
        char host[64];
        char gateway[64];

        if (!parse_route_failure(line, host, gateway))
            continue;

        char route_log[512];
        snprintf(route_log, sizeof(route_log), "%s/route.log", RUN_PATH);

        /* These static routes outlive an IP change, such as airplane mode. A
           route through the old address makes every connection to the P-CSCF
           fail with "Can't assign requested address". */
        int del_status = run_route("delete", host, NULL, route_log);
        int add_status = run_route("add", host, gateway, route_log);
        log_msg("[BBE] IMS_ROUTE: %s via %s (del %d, add %d)", host, gateway, del_status, add_status);
        snprintf(g_route_gateway, sizeof(g_route_gateway), "%s", gateway);
    }

    offset = ftell(f);
    fclose(f);
}
