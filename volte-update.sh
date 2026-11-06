#!/bin/sh
# Updates the VoLTE addon in one step. Copy the new zip to the phone's Downloads or Documents, then
# run this as root:
#   cat volte-update.sh | sh
#   A zip somewhere else: cat volte-update.sh | sh -s /path/to/volte.zip
#   (not "sh volte-update.sh": the shell refuses script files on untrusted paths)
#
# The daemon has to be gone first: with it running, the installer updates the MANIFEST and reports
# success but keeps the old binary. So this stops it, waits, installs, and checks the binary itself.

BIN=/var/addons/volte/init/ims_manager
INSTALLER=/var/rootdata/installer
STAMP=/var/tmp/volte-update.stamp

if [ ! -w /var/addons ]; then
    echo "Cannot write to /var/addons: run this as root. Nothing was changed."
    exit 1
fi

if [ ! -x "$INSTALLER" ]; then
    echo "$INSTALLER is missing: is AutoRoot installed? Nothing was changed."
    exit 1
fi

# The phone has no head. volte-debug bundles may be zipped into the same folders.
ZIP=${1:-$(ls -t /accounts/1000/shared/*/volte*.zip 2>/dev/null | grep -v volte-debug | sed -n 1p)}
if [ ! -f "$ZIP" ]; then
    echo "Cannot find the zip. Copy volte.zip to Downloads and run this again. Nothing was changed."
    exit 1
fi
echo "Using $ZIP"

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

pids=$(daemon_pids)
if [ -n "$pids" ]; then
    echo "Stopping the VoLTE service, this can take up to 25 seconds..."
    kill $pids
    if ! wait_for_daemons 20; then
        kill -9 $(daemon_pids) 2>/dev/null
        if ! wait_for_daemons 5; then
            echo "FAILED: could not stop the VoLTE service. Nothing was installed."
            echo "Reboot the phone and run this again."
            exit 1
        fi
    fi
fi

# File dates have 1 s resolution, and the binary is dated when it's installed.
: > "$STAMP"
sleep 1

# This script arrives on stdin, so the installer must not be able to read it.
"$INSTALLER" "$ZIP" < /dev/null

if [ ! "$BIN" -nt "$STAMP" ]; then
    rm -f "$STAMP"
    echo
    echo "FAILED: the VoLTE program was not updated, whatever the lines above say."
    echo "Reboot the phone and run this again."
    exit 1
fi
rm -f "$STAMP"

echo
echo "SUCCESS: VoLTE updated."
grep '^Package-Version' /var/addons/volte/MANIFEST
ls -l "$BIN"
