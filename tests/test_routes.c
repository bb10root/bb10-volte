/* Run with `make test`. Builds ims_routes.c against a fake ims-start.log and route command. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* The real header needs QNX's slog2. */
#define SYS_UTILS_H
#define RUN_PATH "/tmp/route-test"
int run_cmd_spawn(char *const argv[], const char *log_path);
void log_msg(const char *fmt, ...);

#include "../src/ims_routes.c"

static char g_calls[4][128];
static int g_ncalls;

int run_cmd_spawn(char *const argv[], const char *log_path)
{
    (void)log_path;
    char *out = g_calls[g_ncalls++];
    out[0] = '\0';
    for (int i = 0; argv[i]; i++)
        snprintf(out + strlen(out), 128 - strlen(out), "%s%s", i ? " " : "", argv[i]);
    return 0;
}

void log_msg(const char *fmt, ...)
{
    (void)fmt;
}

int main(void)
{
    char host[64], gateway[64];
    assert(parse_route_failure("add host 192.0.2.1: gateway 198.51.100.1: Permission denied\n", host, gateway));
    assert(strcmp(host, "192.0.2.1") == 0 && strcmp(gateway, "198.51.100.1") == 0);
    assert(parse_route_failure("add host 2a02:ce0:7:c000::c2b4: gateway 2a02:9b0:4106:d1f6:885b:9ef8:5c89:fcc3: Permission denied\n",
                               host, gateway));
    assert(strcmp(host, "2a02:ce0:7:c000::c2b4") == 0 && strcmp(gateway, "2a02:9b0:4106:d1f6:885b:9ef8:5c89:fcc3") == 0);
    assert(!parse_route_failure("delete host 2a02:ce0:7:c000::c2b4: Permission denied\n", host, gateway));
    assert(!parse_route_failure("route: writing to routing socket: Permission denied\n", host, gateway));
    assert(!parse_route_failure("add host 1.2.3.4;rm: gateway 5.6.7.8: Permission denied\n", host, gateway));

    /* The log as Omer's phone wrote it: an IPv6 route needs -inet6. */
    mkdir(RUN_PATH, 0755);
    FILE *f = fopen(RUN_PATH "/ims-start.log", "w");
    assert(f);
    fputs("route: writing to routing socket: Permission denied\n"
          "delete host 2a02:ce0:7:c000::c2b4: Permission denied\n"
          "add host 2a02:ce0:7:c000::c2b4: gateway 2a02:9b0:4106:d1f6:885b:9ef8:5c89:fcc3: Permission denied\n",
          f);
    fclose(f);
    add_missing_ims_routes();
    assert(g_ncalls == 2);
    assert(strcmp(g_calls[0], "route delete -inet6 -host 2a02:ce0:7:c000::c2b4") == 0);
    assert(strcmp(g_calls[1], "route add -inet6 -host 2a02:ce0:7:c000::c2b4 2a02:9b0:4106:d1f6:885b:9ef8:5c89:fcc3") == 0);

    /* The IPv6 gateway above isn't an address of this machine, so it counts as gone and the
       logged routes are cleared first. An IPv4 route keeps the old command. */
    f = fopen(RUN_PATH "/ims-start.log", "w");
    fputs("add host 10.0.0.1: gateway 10.0.0.2: Permission denied\n", f);
    fclose(f);
    g_ncalls = 0;
    add_missing_ims_routes();
    assert(g_ncalls == 3);
    assert(strcmp(g_calls[0], "route delete -host 10.0.0.1") == 0);
    assert(strcmp(g_calls[2], "route add -host 10.0.0.1 10.0.0.2") == 0);

    /* A local IPv6 gateway is kept: no cleanup. */
    assert(address_is_local("::1"));

    remove(RUN_PATH "/ims-start.log");
    rmdir(RUN_PATH);
    puts("route tests passed");
    return 0;
}
