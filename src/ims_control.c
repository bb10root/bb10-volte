#include "ims_control.h"
#include "sys_utils.h"
#include "ims_carrier_config.h"
#include "system_state.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

char g_ims_xml_path[256] = {0};

/* --- Provider & IMS control --- */

void provider_enable(void)
{
    char module_path[512];
    char log_file[512];

    snprintf(module_path, sizeof(module_path), "%s/devnp-volte-swcrypto.so", g_init_path);
    snprintf(log_file, sizeof(log_file), "%s/provider.log", RUN_PATH);

    char *args[] = {
        "mount",
        "-T", "io-pkt",
        "-o", "enable",
        module_path,
        NULL};

    if (run_cmd_spawn(args, log_file) != 0)
        bbe_exit_fail("PROVIDER_FAILED", 1);
}

void stop_ims(void)
{
    if (find_pid_by_name("ims_rrm", 0) > 0)
    {
        if (slay_by_cmdline_graceful("ims_rrm", 15000) != 0)
        {
            bbe_exit_fail("IMS_STOP_FAILED", 1);
        }
        sleep(2);
    }
}

void start_ims(void)
{
    char log_file[512];
    snprintf(log_file, sizeof(log_file), "%s/ims-start.log", RUN_PATH);

    int fd = open(log_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (fd >= 0)
    {
        posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, fd);
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    sigset_t sigdefault;
    sigemptyset(&sigdefault);
    sigaddset(&sigdefault, SIGTERM);
    sigaddset(&sigdefault, SIGINT);

    posix_spawnattr_setsigdefault(&attr, &sigdefault);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);

    char *args[] = {
        "on", "-d", "/radio/bin/ims_rrm",
        "-U", "453:31,80,387,427,521,1001",
        NULL};

    /* `on` execs ims_rrm, so it is our child. Reap one that crashed or was killed. */
    static pid_t last_pid = 0;
    if (last_pid > 0)
        waitpid(last_pid, NULL, WNOHANG);

    pid_t pid;
    int status = posix_spawnp(&pid, "on", &actions, &attr, args, NULL);

    if (status != 0)
    {
        LOG_VERBOSE("Failed to spawn ims_rrm process, error: %d", status);
    }
    else
    {
        last_pid = pid;
        LOG_VERBOSE("Spawned ims_rrm successfully with PID %d", pid);
    }

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (fd >= 0)
    {
        close(fd);
    }
}

void apply_volte_provisioning(void)
{
    if (g_ims_xml_path[0] == '\0')
    {
        if (ims_resolve_config_path(g_mcc, g_mnc, g_gid1, g_ims_xml_path, sizeof(g_ims_xml_path)) != 0)
        {
            snprintf(g_ims_xml_path, sizeof(g_ims_xml_path), "/var/ims/carrier/imsconfig_0%s.xml", g_mnc);
        }
    }

    /* Not fatal: the registration and incoming checks decide the outcome. */
    if (ims_enable_volte_provisioning(g_ims_xml_path) != 0)
    {
        log_msg("[BBE] PROVISIONING_FAILED: %s", g_ims_xml_path);
        return;
    }

    LOG_VERBOSE("VoLTE provisioning applied via XML: %s", g_ims_xml_path);
}

void write_dca_configs(void)
{
    mkdir_p("/var/dca/sources/ims/services/carrier_config/ims", 0755);

    /* The phone's SDP answer copies these lists. Answering a network offer of
       0,2,4,7 (AMR-WB 0,1,2) with only 7 (AMR-WB 2), the stock values, left
       incoming calls with no audio from the phone, and the network ended them
       after about 30 seconds. */
    const char *ir92_cfg = "@ir92\nUsePreconditions:b:true\nSMS_Format::3GPP\namr_modeset_list::0,2,4,7\namrwb_modeset_list::0,1,2\n";
    write_file(IR92_PATH, ir92_cfg, 0644);

    const char *ims_cfg = "@ims\n"
                          "IMS_services:json:[{\"service\":\"voice\",\"feature_tag\":\"urn:urn-7:3gpp-service.ims.icsi.mmtel\",\"network\":[\"LTE\"]}]\n"
                          "Mobility_Management_IMS_Voice_Termination:n:1\n"
                          "IMS_profilesV2:json:[\"IR92\"]\n"
                          "supported_networks:json:[\"LTE\"]\n"
                          "PCSCF_discoveryV2:json:[\"PCO\"]\n"
                          "SDK_Config::3GPP\n"
                          "IMS_always_on:b:true\n"
                          /* ponytail: HD Voice on for every network; add per-carrier
                             handling if one rejects AMR-WB. */
                          "AudioConfig:json:{\"HDvoice\":true}\n"
                          /* REGISTER is about 1900 bytes. Some network routes drop it as
                             fragmented UDP, so send large messages over TCP. */
                          "UDP_max_size:n:1300\n";
    write_file("/var/dca/sources/ims/services/carrier_config/ims/ims", ims_cfg, 0644);

    const char *sources[] = {"ims", "ims_defaults", "omadm", "test"};
    const char *cellular_cfg = "@cellular_protocol_configuration\n"
                               "voice_domain_preference_EUTRAN:n:3\n"
                               "ue_usage_setting::voice_centric\n"
                               "wcdma_to_lte_ps_ho_support:b:false\n";

    char path[512];
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
    {
        snprintf(path, sizeof(path), "/var/dca/sources/%s/services/carrier_config/radio_common", sources[i]);
        mkdir_p(path, 0755);

        snprintf(path, sizeof(path), "/var/dca/sources/%s/services/carrier_config/radio_common/cellular_protocol_configuration", sources[i]);
        write_file(path, cellular_cfg, 0644);
    }

    mkdir_p("/var/dca/sources/test/services/carrier_config/radio_qct", 0755);
    write_file("/var/dca/sources/test/services/carrier_config/radio_qct/ims", "@ims\nIMS_platform::common\n", 0644);
}
