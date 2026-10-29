# BB10 VoLTE Control & Monitoring Daemon

## Overview
`ims_manager` is a low-level background service and utility daemon developed for **BlackBerry 10 OS**. It manages, provisions, and maintains VoLTE (Voice over LTE) connectivity by interacting directly with the QNX Persistent Publish/Subscribe (PPS) service, carrier XML profiles, DCA (Data Carrier Architecture), and the system `ims_rrm` binary.

### Key Operational Features
* **Event-Driven PPS Monitoring:** Real-time multi-descriptor polling (`poll()`) tracking cellular radio technology, SIM readiness, and data connection status.
* **Automatic Reconnection:** Self-healing mechanism with exponential backoff strategy when network conditions fluctuate.
* **Slog2 & File Logging:** Structured logging via QNX `slog2` subsystem and real-time state exports.
* **POSIX Signal Handling:** Graceful termination on `SIGTERM`/`SIGINT` and runtime statistics reset via `SIGUSR1`.
* **CLI Diagnostics:** Native execution flags for status probing, self-tests, and statistic exports.

---

## Architecture & System Flow

```text
                        +-----------------------------------+
                        |         QNX PPS Services          |
                        +-----------------------------------+
                          /               |               \
   /pps/services/cellular/          /pps/.../card0/      /pps/.../cds/cnice/
   radioctrl/status_public          status_private       status_persistent
          (LTE State)                (SIM Status)          (Data Mode)
               \                          |                      /
                \                         |                     /
                 +------------------------+--------------------+
                                          |
                                    poll() [FD_COUNT=3]
                                          v
                        +-----------------------------------+
                        |   detailed_pps_monitor_thread()   |
                        +-----------------------------------+
                                          |
                      System State Change (Ready <-> Not Ready)
                                          |
                     +--------------------+--------------------+
                     |                                         |
             [State: Lost]                             [State: Restored]
                     |                                         |
                     v                                         v
               stop_ims()                         reconnect_ims_with_backoff()
            (Graceful slay)                                    |
                     |                                 apply_volte_provisioning()
                     v                                         |
              stats_on_drop()                              start_ims()
                                                               |
                                                        ims_connected_pps()
                                                               |
                                                    +----------+----------+
                                                    |                     |
                                                [Success]              [Failed]
                                                    |                     |
                                                    v                     v
                                           stats_on_success()    stats_on_failure()
```

---

## Prerequisites & Dependencies

### Headers & Libraries
* **QNX Neutrino C Library:** Standard POSIX routines (`pthread`, `poll`, `spawn`).
* **PPS Library:** `<sys/pps.h>` (`libpps.so`)
* **Slog2 Subsystem:** `<sys/slog2.h>` (`libslog2.so`)

### Module Internal Headers
* `sys_utils.h`: System wrappers, string manipulators, and POSIX spawn execution functions.
* `berry_monitor.h`: Visual indicator and UI status bar integration utilities.
* `ims_carrier_config.h`: XML carrier profile parser and provisioning apply utilities.

### Monitored PPS Paths

| Attribute Identifier | Monitored PPS Object Path | Valid Condition |
| :--- | :--- | :--- |
| **Radio Technology** | `/pps/services/cellular/radioctrl/status_public` | `network_technology` contains `"lte"` |
| **SIM Card Status** | `/pps/services/cellular/uicc/card0/status_private` | `uicc_state` contains `"READY"` |
| **Data Service Mode** | `/pps/services/cds/cnice/status_persistent` | `data_service_mode` contains `"ON"` |
| **Voice Call State** | `/pps/services/phone/public/status` | `phone_state` contains `"idle"` |

---

## CLI Options & Execution Syntax

```bash
ims_manager [options]
ims_manager [action] [refresh]
```

### Options Overview

| Parameter | Argument | Description |
| :--- | :--- | :--- |
| `-a, --action` | `<check\|probe\|enable>` | Selects operational mode (default: `check`). |
| `-r, --refresh` | *None* | Forces DCA and IMS config re-provisioning during enable. |
| `-v, --verbose` | *None* | Enables verbose output to `stdout`/`slog2`. |
| `--self-test` | *None* | Executes unit testing suite for internal PPS parsers. |
| `--berry-check` | *None* | Inspects current IMS registration vs Berry indicator UI state. |
| `--stats` | *None* | Outputs the formatted statistics file content from `/var/run/bb10_volte_stats.txt`. |
| `--reset-stats` | *None* | Clears runtime statistical counters and regenerates the stats file. |
| `-h, --help` | *None* | Displays execution options and usage text. |

---

## POSIX Signal Interface

* **`SIGTERM` / `SIGINT`:** Intercepts termination requests, stops background threads safely (`stop_lte_monitor()`, `stop_berry()`), closes file descriptors, and terminates cleanly.
* **`SIGUSR1`:** Triggers a non-destructive runtime reset of connection statistics counters without restarting the process.

**Example signaling via shell:**
```bash
# Reset runtime connection statistics
kill -SIGUSR1 $(pidof ims_manager)
```

---

## Exported Files & Output Artifacts

* `/var/run/bb10_volte_stats.txt`: Contains current session statistics including total drops, successful/failed reconnection attempts, and average recovery duration.
* `/var/run/provider.log`: Execution logs for `devnp-volte-swcrypto.so` network driver stack mounting.
* `/var/run/ims-start.log`: Output and error logs generated by `/radio/bin/ims_rrm`.
* `/var/run/dca-enable.log`: Execution trace from the `dca` service provisioning command.

---
