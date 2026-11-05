#include "self_test.h"
#include "berry_monitor.h"
#include "ims_routes.h"
#include "voice_flag.h"

#include <stdio.h>
#include <string.h>

int self_test(void)
{
    const struct
    {
        const char *pps;
        int want;
    } cases[] = {
        {"@status_public\n[n]status::connected\n[n]default_ran::LTE\ndeviceConfig:json:[\"VoLTE\"]\n", 1},
        {"@status_public\nstatus::disconnected\ndefault_ran::LTE\ndeviceConfig:json:[\"VoLTE\"]\n", 0},
        {"@status_public\nstatus::connected\ndefault_ran::WLAN\ndeviceConfig:json:[\"VoLTE\"]\n", 0},
        {"@status_public\nstatus::connected\ndefault_ran::LTE\ndeviceConfig:json:[\"SMS\",\"VoLTE\"]\n", 1},
        {"@status_public\nstatus::connected\ndefault_ran::LTE\ndeviceConfig:json:[\"SMS\"]\n", 0},
        {"@status_public\nstatus::connected\ndefault_ran::LTE\ndeviceConfig:json:[\"VoLTE\"\n", 0},
        {"@status_public\n", 0}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", cases[i].pps);
        int active = parse_ims_pps_buffer(buf);

        if (active != cases[i].want)
        {
            fprintf(stderr, "[BBE] PPS decoder self-test %zu failed\n", i);
            return 1;
        }
    }

    puts("[BBE] PPS decoder: 7 cases passed");
    return 0;
}

int voice_flag_self_test(void)
{
    const struct
    {
        unsigned long service;
        int ims_up;
        unsigned long want;
    } cases[] = {
        {0x20040AUL, 1, 0x220040AUL}, /* LTE, IMS up: set */
        {0x220040AUL, 1, 0x220040AUL}, /* Already set: unchanged */
        {0x220040AUL, 0, 0x20040AUL}, /* IMS down: clear for CS fallback */
        {0x2002UL, 1, 0x2002UL},      /* GSM: never set */
        {0x2002002UL, 1, 0x2002UL}};  /* Left over on GSM: clear */

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        if (next_network_service(cases[i].service, cases[i].ims_up) != cases[i].want)
        {
            fprintf(stderr, "[BBE] voice flag self-test %zu failed\n", i);
            return 1;
        }
    }

    puts("[BBE] Voice flag: 5 cases passed");

    char host[64];
    char gateway[64];
    if (!parse_route_failure("add host 192.0.2.1: gateway 198.51.100.1: Permission denied\n", host, gateway) ||
        strcmp(host, "192.0.2.1") != 0 || strcmp(gateway, "198.51.100.1") != 0 ||
        parse_route_failure("route: writing to routing socket: Permission denied\n", host, gateway) ||
        parse_route_failure("add host fd00::1: gateway fd00::2: Permission denied\n", host, gateway))
    {
        fprintf(stderr, "[BBE] route parser self-test failed\n");
        return 1;
    }

    puts("[BBE] Route parser: 3 cases passed");
    return 0;
}
