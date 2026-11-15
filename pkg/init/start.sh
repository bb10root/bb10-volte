#!/bin/sh
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BIN=$SCRIPT_DIR/ims_manager
LOG=/var/log/installer.log
REFRESH=

# Only there so the installer prints the version (see the Makefile); older installs leave theirs.
VER=$(sed -n 's/^Package-Version: *//p' "$SCRIPT_DIR/../MANIFEST")
for f in "$SCRIPT_DIR"/version-*; do
    [ "$f" = "$SCRIPT_DIR/version-$VER" ] || rm -f "$f"
done

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
        # A new build may write different settings or patch bytes. -r applies them even when IMS
        # is already registered, so an update needs no reboot.
        REFRESH=-r
        echo "$(date '+%Y-%m-%d %H:%M:%S') [BBE] UPDATED: $(grep '^Package-Version' "$SCRIPT_DIR/../MANIFEST")" >> "$LOG"
    else
        echo "$(date '+%Y-%m-%d %H:%M:%S') [BBE] UPDATE_FAILED: could not move the new binary" >> "$LOG"
    fi
fi

# Trust is on the inode and a moved file doesn't keep it, so trust it on every start, boot included.
# The daemon does the same for its ims_rrm copy.
/proc/boot/pathtrust "!$BIN"
# Detached from whoever ran this script (boot, a terminal, the VoLTE Domain app): -t gives a new
# session with pid 1 as parent and no stdio of theirs, so the daemon never writes to a pipe whose
# reader has gone. The daemon itself closes any other descriptors it inherited and, when started
# from an app (the VoLTE Domain installer), takes a new appid, so closing that app can't end it.
on -d -t /dev/null "$BIN" -a enable $REFRESH
