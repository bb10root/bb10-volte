#!/bin/sh
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BIN=$SCRIPT_DIR/ims_manager
LOG=/var/log/installer.log

daemon_pids() {
    pidin arg 2>/dev/null | grep '[i]ms_manager' | while read -r pid prog rest; do
        case $prog in ims_manager|*/ims_manager) echo "$pid" ;; esac
    done
}

# kill returns before the daemon has exited, and it may take 15 s to stop ims_rrm.
wait_for_daemons() {
    waited=0
    while [ -n "$(daemon_pids)" ]; do
        [ "$waited" -ge "$1" ] && return 1
        sleep 1
        waited=$((waited + 1))
    done
    return 0
}

# The installer can't replace a running binary: it keeps the old one and still reports success.
# So the package also carries the daemon as ims_manager.new, which it can always write, and the
# installer runs this script once the files are copied.
if [ -f "$BIN.new" ]; then
    pids=$(daemon_pids)
    if [ -n "$pids" ]; then
        kill $pids
        if ! wait_for_daemons 20; then
            kill -9 $(daemon_pids) 2>/dev/null
            wait_for_daemons 5
        fi
    fi
    if [ -n "$(daemon_pids)" ]; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') [BBE] UPDATE_FAILED: old daemon still running, reboot" >> "$LOG"
        exit 1
    fi
    if mv -f "$BIN.new" "$BIN"; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') [BBE] UPDATED: $(grep '^Package-Version' "$SCRIPT_DIR/../MANIFEST")" >> "$LOG"
    else
        echo "$(date '+%Y-%m-%d %H:%M:%S') [BBE] UPDATE_FAILED: could not move the new binary" >> "$LOG"
    fi
fi

# Trust is on the inode and a moved file doesn't keep it, so trust it on every start, boot included.
# The daemon does the same for its ims_rrm copy.
/proc/boot/pathtrust "!$BIN"
on -d "$BIN" -a enable
