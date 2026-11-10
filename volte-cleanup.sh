#!/bin/sh
# Clears what can get in the way of a clean VoLTE install. Run as root:
#   cat volte-cleanup.sh | sh             shows what it would do and changes nothing
#   cat volte-cleanup.sh | sh -s apply    does it, keeping a copy of what it removes
#   (not "sh volte-cleanup.sh": the shell refuses script files on untrusted paths)
# After apply: reboot, install volte.zip with /var/rootdata/installer, reboot again.
#
# Removes: running ims_manager daemons, /var/addons/volte, any other addon carrying this addon's
# files, /var/rootdata/volte, the cdsc_xml override, /var/ims/carrier/imsconfig_*.xml newer than
# the OS build, and old runtime logs. It never removes stock files.
# Also removes the third-party bb10-volte-vn addon and everything it leaves, because its boot script
# deletes /var/addons/volte. If its service is installed, its own uninstall runs first, since only
# that service's journal holds the IMS settings and voice policy it replaced.
# The installer copies over an existing folder and never deletes, and a leftover
# carrier-<id>-volte.xml would beat the profile the daemon builds, so the folder has to go.
# ims_rrm reads and saves imsconfig_<carrier id>.xml, and imsconfig_user.xml can override the SIM's
# identities. It may write them again later, starting from the current settings.
#
# Leaves alone: other addons, /var/dca/sources, and any bb10-volte-vn zip in the shared folders, which
# is only listed: reinstalling it would remove this addon again. The daemon overwrites the stock DCA files with
# no backup and rewrites them when it applies its config, so this is not an uninstaller: it does
# not bring back the stock IMS settings.

# Some root shells can't run grep from their own PATH, and a failed grep would read as "nothing found".
PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin:$PATH

MODE=${1:-check}
case $MODE in
    check|apply) ;;
    *) echo "Unknown option: $MODE. Use apply, or nothing to only look."; exit 1 ;;
esac

if [ ! -w /var/addons ]; then
    echo "Cannot write to /var/addons: run this as root."
    exit 1
fi

PPS_DATA=/pps/services/cds/cnice/status_persistent
PATHTRUST=/proc/boot/pathtrust
PY=/usr/bin/python3.2
VN_BASE=/var/rootdata/q10_volte
VN_PENDING=/var/rootdata/bb10-volte-cleanup-pending
# The brackets stop grep from matching its own line in pidin.
VN_SCRIPTS='[b]b10-volte[.]sh|[a]ddon-start[.]sh'
VN_SERVICE='[q]10_volte'
BACKUP=/accounts/1000/shared/documents/volte-cleanup-backup-$(date +%Y%m%d-%H%M%S)
FAILED=0

# Checked before anything changes, so a failed backup leaves the phone as it was.
if [ "$MODE" = apply ] && ! mkdir -p "$BACKUP"; then
    echo "Cannot create $BACKUP. Nothing was changed."
    exit 1
fi

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

remove() {
    echo "$1:"
    ls -lR "$1"
    if [ "$MODE" != apply ]; then
        echo "  would remove it"
        return
    fi

    copy=$BACKUP$1
    if ! mkdir -p "${copy%/*}" || ! cp -R "$1" "$copy"; then
        echo "  could not back it up, so it was left in place"
        FAILED=1
        return
    fi

    if rm -rf "$1"; then
        echo "  removed, copy in $copy"
    else
        echo "  could not remove it"
        FAILED=1
    fi
}

pids_matching() {
    pidin arg 2>/dev/null | grep -E "$1" | while read -r pid rest; do echo "$pid"; done
}

stop_matching() {
    [ -n "$(pids_matching "$1")" ] || return 0
    kill $(pids_matching "$1") 2>/dev/null
    waited=0
    while [ -n "$(pids_matching "$1")" ] && [ "$waited" -lt 10 ]; do
        sleep 1
        waited=$((waited + 1))
    done
    [ -z "$(pids_matching "$1")" ] || kill -9 $(pids_matching "$1") 2>/dev/null
}

# Same environment as its bb10-volte.sh. Its files lose their trust at every reboot.
vn_uninstall() {
    for file in "$VN_BASE"/device/*; do
        "$PATHTRUST" "!$file"
    done
    (
        cd / &&
            LD_LIBRARY_PATH=/base/usr/lib:/base/lib:/base/lib/dll:/radio/lib:/radio/lib/dll:/radio/usr/lib:/proc/boot \
            PYTHONHOME=/usr PYTHONPATH=/usr/lib/python32.zip:/usr/lib/python3.2/lib-dynload \
            "$PY" "$VN_BASE/device/install.py" uninstall
    )
}

is_vn_addon() {
    [ -e "$1/data/bb10-volte.sh" ] || [ -e "$1/data/volte-enabler-cleaner.sh" ] ||
        [ "${1##*/}" = q10-unicom-volte ] || [ "${1##*/}" = bb10-volte-vn ]
}

# Only this addon's own files count as a conflict, since other addons are none of our business.
is_this_addon() {
    [ -e "$1/init/ims_manager" ] ||
        [ -e "$1/init/devnp-volte-swcrypto.so" ] ||
        grep -q '^Package-Id:[ ]*volte[^a-zA-Z0-9_-]*$' "$1/MANIFEST" 2>/dev/null
}

echo "--- running daemons"
pids=$(daemon_pids)
if [ -z "$pids" ]; then
    echo "none"
elif [ "$MODE" != apply ]; then
    echo "would stop:" $pids
else
    kill $pids
    if ! wait_for_daemons 20; then
        kill -9 $(daemon_pids) 2>/dev/null
        if ! wait_for_daemons 5; then
            echo "Could not stop:" $(daemon_pids) ". Nothing was removed. Reboot and try again."
            exit 1
        fi
    fi
    echo "stopped:" $pids
fi

echo "--- bb10-volte-vn"
if [ "$MODE" != apply ]; then
    found=$(pids_matching "$VN_SCRIPTS|$VN_SERVICE")
    [ -n "$found" ] && echo "would stop:" $found
    [ -f "$VN_BASE/installation.json" ] && echo "would run its uninstall, which restores the IMS settings it changed"
    [ -n "$found" ] || [ -f "$VN_BASE/installation.json" ] || echo "none"
else
    stop_matching "$VN_SCRIPTS"
    # If its uninstall fails below, this keeps it from removing this addon again at the next boot.
    for dir in /var/addons/*; do
        is_vn_addon "$dir" && [ -d "$dir/data" ] && touch "$dir/data/no-auto-install"
    done
    rm -f "$VN_PENDING"
    if [ -f "$VN_BASE/installation.json" ]; then
        if vn_uninstall; then
            echo "its uninstall finished"
        else
            echo "Its uninstall failed: see above. It was told not to reinstall itself, and its files were"
            echo "kept, because its journal is the only record of the original IMS settings."
            echo "IMS may still be running: wait 6 minutes after boot and try again. If this repeats, send this output."
            exit 1
        fi
    fi
    stop_matching "$VN_SERVICE"
fi

echo "--- cdsc_xml override"
xml=$(grep '^cdsc_xml::' "$PPS_DATA" 2>/dev/null | tail -n 1)
xml=${xml#cdsc_xml::}
if [ -z "$xml" ]; then
    echo "none"
elif [ "$MODE" != apply ]; then
    echo "would clear: $xml"
else
    [ -f "$xml" ] && cp "$xml" "$BACKUP/"
    echo "cdsc_xml::" >> "$PPS_DATA"
    now=$(grep '^cdsc_xml::' "$PPS_DATA" | tail -n 1)
    if [ "$now" = "cdsc_xml::" ]; then
        echo "cleared: $xml"
    else
        echo "could not clear: $xml"
        FAILED=1
    fi
fi

echo "--- addons"
for dir in /var/addons/*; do
    [ -d "$dir" ] || continue
    if [ "$dir" = /var/addons/volte ] || is_this_addon "$dir" || is_vn_addon "$dir"; then
        grep '^Package-Version' "$dir/MANIFEST" 2>/dev/null
        remove "$dir"
    elif grep -qE 'ims_rrm|cdsc_xml|carrier_config/ims' "$dir/init/start.sh" "$dir/MANIFEST" 2>/dev/null; then
        echo "$dir: kept, but it mentions IMS. Check it by hand if the problem remains."
    else
        echo "$dir: kept"
    fi
done

echo "--- leftovers"
for path in /var/rootdata/volte /var/tmp/bb10-volte /var/run/bb10_volte_stats.txt \
    "$VN_BASE" "$VN_BASE"_stage_* "$VN_PENDING" /var/rootdata/volte-enabler-backup-* \
    /var/rootdata/bb10-volte-addon.log /var/rootdata/.bb10-volte-root-test /var/rootdata/.cleaner-root-test \
    /var/rootdata/.volte-enabler-cleaner-voice.py /pps/volte_probe \
    /accounts/1000/shared/downloads/bb10-volte-boot-trace.txt /accounts/1000/shared/downloads/bb10-volte-diag-*.txt; do
    [ -e "$path" ] && remove "$path"
done

# Stock files keep the OS build date, and the daemon never rewrites this one. If the date can't be
# compared, the file stays.
STOCK_REF=/var/dca/sources/ims/services/carrier_config/ims/emergency
for path in /var/ims/carrier/imsconfig_*.xml; do
    [ -e "$path" ] || continue
    if [ -e "$STOCK_REF" ] && [ "$path" -nt "$STOCK_REF" ]; then
        remove "$path"
    else
        echo "$path: kept, it may be stock"
    fi
done

echo "--- installer"
if [ -x /var/rootdata/installer ]; then
    echo "/var/rootdata/installer is present"
else
    echo "/var/rootdata/installer is MISSING: volte.zip cannot be installed without it"
fi

# Another patch can leave files here too, but stock files live alongside them with no copy to
# restore, so only list them. Stock DCA files are dated when the OS was built.
echo "--- for review, not changed"
ls -l /var/rootdata /var/ims /var/radio/cdsc 2>&1
ls -l /accounts/1000/shared/*/bb10-volte-vn* /accounts/1000/shared/*/volte-enabler-cleaner.sh \
    /accounts/1000/shared/*/volte-diag.sh 2>/dev/null
find /var/dca/sources -type f 2>/dev/null | while read -r file; do
    ls -l "$file"
done

echo
if [ "$MODE" != apply ]; then
    echo "Nothing was changed. To do this: cat volte-cleanup.sh | sh -s apply"
    exit 0
fi

# Files are owned by root; this lets the normal user read and delete them.
chmod -R g+rwX "$BACKUP"

if [ "$FAILED" != 0 ]; then
    echo "Some steps failed: see above. Copies are in $BACKUP"
    exit 1
fi

echo "Done. Copies are in $BACKUP"
echo "They can hold APN logins and SIM identities: do not send them to anyone as they are."
echo "Next: reboot, run /var/rootdata/installer /path/to/volte.zip and keep its output, reboot again."
