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

/* ponytail: IPv4 only; extend the character classes if a network gives IPv6 P-CSCFs. */
int parse_route_failure(const char *line, char *host, char *gateway)
{
    return sscanf(line, "add host %63[0-9.]: gateway %63[0-9.]: Permission denied", host, gateway) == 2;
}

/* ims_rrm can't add its P-CSCF routes without root, and reports each failure in
 * its log. Without them, replies to connections from the P-CSCF leave on the
 * default route, so the phone never receives a BYE sent over TCP. */
static char g_route_gateway[64] = "";

static int address_is_local(const char *address)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (inet_pton(AF_INET, address, &sa.sin_addr) != 1)
        return 1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;

    int gone = bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno == EADDRNOTAVAIL;
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

        char *del_args[] = {"route", "delete", "-host", host, NULL};
        if (run_cmd_spawn(del_args, route_log) == 0)
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
        char *del_args[] = {"route", "delete", "-host", host, NULL};
        char *add_args[] = {"route", "add", "-host", host, gateway, NULL};
        int del_status = run_cmd_spawn(del_args, route_log);
        int add_status = run_cmd_spawn(add_args, route_log);
        log_msg("[BBE] IMS_ROUTE: %s via %s (del %d, add %d)", host, gateway, del_status, add_status);
        snprintf(g_route_gateway, sizeof(g_route_gateway), "%s", gateway);
    }

    offset = ftell(f);
    fclose(f);
}
