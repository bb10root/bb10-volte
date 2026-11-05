# Handoff: slow IMS reconnect in bb10-volte

Written 2026-11-05.

## The task

After a drop, IMS takes 82 to 102 s to reconnect, and during that time incoming calls can't reach IMS. Find out why the first reconnect attempt stalls, then shorten the reconnect in the daemon.

The leading cause comes from the logs and is not yet tested. The first attempt's protected `REGISTER` fails with `Address already in use` on local port 50000, probably because the previous TCP connection is still in `TIME_WAIT`.

Done means:

- The cause is confirmed or replaced, with log evidence.
- A typical reconnect is measurably shorter, read from `STATS_OK: last=` on the phone. Set the target once the real floor is known.
- Normal calls still work in both directions after a reconnect.
- The change is small and sits in the daemon. Nothing is committed unless the user asks.

## Evidence so far

The first attempt almost always wastes its full 60 s, because its one failed send is never retried. `errno 248` appears once in each of 14 saved log files, covering at least 9 separate restarts, and the second attempt then registers in about 2 s.

The clearest trace is the reconnect of 2026-11-05 after a 2G call. Times are phone time, and `last=` counts from the drop.

| Time | Event | Source |
| --- | --- | --- |
| 15:44:10 | LTE lost for a 2G call, `DROPPED: LTE_LOST` | daemon log |
| 15:44:28.9 | LTE back, `RECONNECT_ATTEMPT:1` | daemon log |
| 15:44:30.5 | First `REGISTER` gets `401`, which is normal | `ims_rrm` log |
| 15:44:30.579 | Protected `REGISTER`: `Binding local port to 50000`, then `connect returned -1, errno 248 'Address already in use'` and `privateSend() failed` | `ims_rrm` log |
| 15:44:32 | `netstat` shows `10.96.59.62.50000 10.133.101.23.6000 TIME_WAIT` | `netstat` |
| 15:45:21 | That `TIME_WAIT` entry is gone, so it lasted 21 to 70 s, counting from `stop_ims()` at the drop | `netstat` |
| 15:45:29 | `wait_connected()` gives up after 60 s. `ims_rrm` logged no retry in between | daemon log |
| 15:45:31 | `RECONNECT_ATTEMPT:2` | daemon log |
| 15:45:48.8 | New `ims_rrm` starting, about 17 s after attempt 2 began | daemon log |
| 15:45:51 | `200 OK`, connected about 2 s after that | `ims_rrm` log |
| 15:45:52.9 | `CONNECTED`, `STATS_OK: last=102.41s` | daemon log |

Where the time goes on a typical `last=82` s restart: 60 s in `wait_connected()`, 2 s of backoff, and about 20 s between attempt 2 starting and `CONNECTED`. The same pattern shows at 10:40, 10:58 and 11:12 that day.

A separate failure looks similar but has a different cause. After airplane mode on 2026-11-05 the phone's IP changed, and static P-CSCF routes through the old address made every connect fail with `errno 249 'Can't assign requested address'`, so all 6 attempts failed. The daemon now deletes the old route before adding the new one, in `add_missing_ims_routes()`. If you see `errno 249` on the P-CSCF connection, check `route -n get 10.133.101.23` first.

That run also supports the `TIME_WAIT` theory. After removing the routes by hand, at 16:02:17, more than 10 minutes after the airplane toggle, attempt 1 registered at once with no `errno 248`. It still spent 17 s before starting `ims_rrm`.

**Proven:** the `errno 248` failure, the missing retry within the 60 s, and the second attempt succeeding.

**Inferred, not proven:** that `TIME_WAIT` from the previous `ims_rrm` causes the `errno 248`. The port and timing fit, but nobody has tested removing it.

**Unexplained:**

- Why attempt 2 spends about 17 s before starting `ims_rrm`.
- How the rarer fast reconnects happened (`STATS_OK: last=2.17s` and `2.32s` in older logs). They may be cases with no leftover connection.

## Code to read

The reconnect logic is all in `src/ims_manager.c`, in the `reconnect_ims_with_backoff()` loop. Line numbers are from the current uncommitted tree.

| Location | What it does | Why it matters |
| --- | --- | --- |
| `ims_manager.c:37` | `REGISTER_WAIT_SEC 60` | The wait that a stalled first attempt uses up |
| `ims_manager.c:30-32` | Backoff starts at 2 s, maximum 6 attempts | The 2 s gap between attempts |
| `ims_manager.c:456` `stop_ims()` | Graceful stop with a 15 s limit, then `sleep(2)` | 15 s + 2 s matches the unexplained 17 s, so check whether the stop always hits its limit |
| `ims_manager.c:468` `start_ims()` | Runs `on -d /radio/bin/ims_rrm` | Starts the process that binds port 50000 |
| `ims_manager.c:641` `wait_connected()` | Polls every 2 s, and adds missing P-CSCF routes | Can't see the `errno 248`, because it only watches IMS status in PPS |
| `ims_manager.c:712` `reconnect_ims_with_backoff()` | Stop, provision, start, wait, back off | The main place to change |
| `ims_manager.c:775` `check_ims_health()` | Allows `IMS_DOWN_GRACE_SEC` (60 s) before reconnecting | Adds a further 60 s when IMS is lost with LTE still up |
| `ims_manager.c:996` | State-change path: stops IMS on a drop, reconnects on restore | The path in the trace above |
| `sys_utils.c:310` `slay_by_cmdline_graceful()` | Finds processes by command line and waits for them to exit | The likely source of the 15 s |

`ims_rrm` is a closed BlackBerry binary at `/radio/bin/ims_rrm`, and it should stay stock. A local copy for disassembly can be pulled from the phone. The earlier work used `/root/bbndk/host_10_3_1_12/linux/x86/usr/bin/arm-unknown-nto-qnx8.0.0eabi-objdump -d -M force-thumb`, inside the Docker image `yamsergey/bb10-ndk:0.6`.

## Hypotheses and fix options

Confirm the cause first, then take the cheapest option that holds. The two delays are separate, so fixing the 17 s stop is worthwhile whatever happens with the 60 s.

**Confirm the cause.** Just before each reconnect, record whether `netstat -an` shows a `TIME_WAIT` entry on local port 50000. The theory predicts `errno 248` when the entry is there and a 2 s registration when it isn't. If registration still fails with no entry, the theory is wrong. A rare fast reconnect in `STATS_OK` is worth comparing too.

| Option | Idea | Likely saving | Check or risk |
| --- | --- | --- | --- |
| Fix the stop delay | Find out why `stop_ims()` takes 17 s when `ims_rrm` logs its exit almost at once. The `on -d` wrapper or a match on the wrong process may keep the wait running | About 15 s | Low risk. A stuck stop must still be handled |
| Wait out `TIME_WAIT` | After `stop_ims()`, poll until the port 50000 entry is gone, then start `ims_rrm` | Skips the wasted 60 s and second attempt, leaving a reconnect of up to about the `TIME_WAIT` length | Small change. Parsing `netstat` is crude, and it needs a time limit |
| Change the port | Look for a config setting behind local ports 50000 and 50001, so a new start doesn't collide | Removes the wait | Unknown whether a setting exists. Check `ims_rrm` strings and `Reserved ports` in the logs |
| Shorten `TIME_WAIT` | Tune the TCP stack | Removes most of the wait | `sysctl net.inet.tcp.msl` is rejected on this phone. Other setting names are unchecked, and it affects the whole phone |
| Don't stop `ims_rrm` on a drop | Let it re-register by itself when LTE returns | Could avoid the problem | Highest risk. The code notes that a connection rejected on 2G can stay stalled, and the stop at `ims_manager.c:996` may exist for that reason |
| Lower `REGISTER_WAIT_SEC` | Give up on the first attempt sooner | Up to the amount cut | Not recommended alone. Attempt 2 would still collide while `TIME_WAIT` lasts, and slow real registrations would be cut off |

The user's standing preference is a root-cause fix, not a trick that hides the symptom. They rejected a hold-and-resume workaround for the audio bug on those grounds.

## How to test on the phone

The test phone is a BlackBerry Q10 on CoopVoce, reached over USB with the SSH alias `bb10rootx`. It is read-only for agents: give the user exact commands for anything that writes, and let them run those.

**Allowed without asking:** `slog2info`, `netstat`, `pidin`, `cat`, `grep`, `cksum`, and `tcpdump`. Don't create temp files on the phone, even in `/var/tmp`.

**The user triggers each reconnect**, by toggling airplane mode or running `slay ims_rrm`. Those two take different paths: airplane mode goes through the state-change path, and `slay` goes through `check_ims_health()`. Test both.

Pull the logs and connection state:

```
ssh bb10rootx 'date +%H:%M:%S; netstat -an | grep -E "5060|50000|50001|6000"; slog2info' > now.txt
/usr/bin/grep -a -E '\[BBE\]|errno 248' now.txt | tail -40
```

Build, then give the user the install commands:

```
docker run --rm -v "$PWD":/w -w /w yamsergey/bb10-ndk:0.6 bash -lc 'source /root/bbndk/bbndk-env_10_3_1_995.sh >/dev/null 2>&1; make 2>&1 | tail -15'

scp volte.zip bb10rootx:/var/tmp/volte.zip
ssh bb10rootx 'slay ims_manager; while pidin arg | grep "[i]ms_manager" > /dev/null; do sleep 1; done; /var/rootdata/installer /var/tmp/volte.zip; cksum /var/addons/volte/init/ims_manager'
cksum pkg/init/ims_manager
```

The two checksums must match, then the user reboots.

**Traps that cost time before:**

- **Logs vanish when `ims_rrm` exits.** Its slog buffer goes with the process, so a missing `REGISTER` proves nothing. Save logs before the next stop or restart, or poll during the reconnect.
- **Mac `grep` is a shell function** that silently fails on slog files, which contain NUL bytes. Use `/usr/bin/grep -a`.
- **`CONNECTED` and the voice flag can look ready early.** Also check for an `ESTABLISHED` inbound TCP entry on port 50001 before asking for an incoming call.
- **`slay ims_manager` is asynchronous.** If the installer runs too early, it reports success but keeps the old binary.
- **Phone tools are limited.** There is no `head`, `tr`, `uptime`, `printf` or `paste`. `grep -c` on binaries gives false negatives, so pull the binary and search it on the Mac.
- **Phone numbers and SIP identities appear in logs.** Sanitize them before showing logs or writing them anywhere.
- **The Q10 can hang.** It hung once during a call, with SSH dead while ping still answered, and needed a battery pull. The cause is unknown and the logs were lost.

## Constraints and current state

The tree holds important uncommitted work, so build on it and don't reset, stash or revert it. The repo is `/Users/andreafaggin/own-projects/bb10/bb10-volte`, on branch `feature/more`, and the last commit is `6534d62`.

**Uncommitted and working:**

- 2.8 and 2.9 changes to `src/ims_manager.c`, `pkg/MANIFEST` and `pkg/data/perms`.
- The audio fix in `write_dca_configs()`: `amr_modeset_list::0,2,4,7`, `amrwb_modeset_list::0,1,2` and `AudioConfig:json:{"HDvoice":true}`. Don't change these. They fixed silent incoming calls, and the installed build was checked after a reboot with one call in each direction.

**Rules:**

- Commit or push only when the user asks. The user may commit the existing work first, so check `git status` and `git log` before starting.
- Never commit `pkg/data/carrier-default-volte.xml`, `pkg/data/force-ims-voice` or their `perms` lines without asking. They are personal, and the built `volte.zip` includes them, so it isn't for sharing.
- Don't commit this handoff file unless the user asks.
- Keep `/radio/bin/ims_rrm` stock in the final fix. Patched copies were for diagnosis only.
- Keep the fix small and in the daemon, and leave one small runnable check for any new parsing or branching logic. `ims_manager` already has a self-test mechanism.

**Related but out of scope:**

- One incoming call about 58 s after a post-reboot `CONNECTED` went to 2G, and the cause is unknown. Earlier incoming calls fell back to 2G the same way, and those are unexplained too. Report anything you notice, but don't chase it. A 2G fallback is also what triggered the reconnect traced above.
- The airplane mode toggle still needs a retest with the stale route fix.
- A Q20 will be available later for testing on another model.
- Moving the self-tests out of `ims_manager.c` and splitting the daemon is planned for later.

The full history is in the project memory file `q10-coopvoce-volte-findings.md`, which covers the findings on this phone and network. Check anything from it against the current code and logs before relying on it.
