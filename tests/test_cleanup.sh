#!/bin/sh
# Runs volte-cleanup.sh against a fake phone in a temp folder. No phone needed.
#   sh tests/test_cleanup.sh       or: SH=ksh sh tests/test_cleanup.sh

SH=${SH:-sh}
SCRIPT=$(dirname "$0")/../volte-cleanup.sh
R=$(mktemp -d)
FAILS=0

check() {
    if eval "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        FAILS=$((FAILS + 1))
    fi
}

make_phone() {
    rm -rf "$R/var" "$R/pps" "$R/accounts"
    mkdir -p "$R/var/addons/volte/init" "$R/var/addons/volte/data" \
        "$R/var/addons/renamed/init" "$R/var/addons/keys/init" "$R/var/addons/ims-tool/init" \
        "$R/var/rootdata/volte" "$R/var/tmp/bb10-volte" "$R/var/run" "$R/var/ims/carrier" \
        "$R/var/dca/sources/ims" "$R/pps/services/cds/cnice" "$R/accounts/1000/shared/documents"
    echo 'Package-Version: 2.0' > "$R/var/addons/volte/MANIFEST"
    : > "$R/var/addons/volte/init/ims_manager"
    : > "$R/var/addons/volte/data/carrier-182-volte.xml"
    : > "$R/var/addons/renamed/init/devnp-volte-swcrypto.so"
    echo 'Package-Id: keys' > "$R/var/addons/keys/MANIFEST"
    echo 'echo ims_rrm' > "$R/var/addons/ims-tool/init/start.sh"
    echo '<carrier/>' > "$R/var/rootdata/volte/data-profile.xml"
    : > "$R/var/rootdata/installer"
    chmod +x "$R/var/rootdata/installer"
    : > "$R/var/tmp/bb10-volte/route.log"
    : > "$R/var/run/bb10_volte_stats.txt"
    : > "$R/var/ims/carrier/imsconfig_182.xml"
    : > "$R/var/ims/carrier/imsconfig_-01.xml"
    : > "$R/var/ims/carrier/imsconfig_user.xml"
    : > "$R/var/ims/carrier/contacts-not-ours"
    : > "$R/var/dca/sources/ims/ir92"
    mkdir -p "$R/var/dca/sources/ims/services/carrier_config/ims"
    : > "$R/var/ims/carrier/imsconfig_default.xml"
    : > "$R/var/dca/sources/ims/services/carrier_config/ims/emergency"
    touch -t 201802211054 "$R/var/ims/carrier/imsconfig_default.xml" \
        "$R/var/dca/sources/ims/services/carrier_config/ims/emergency"
    {
        echo 'data_service_mode::ON'
        echo "cdsc_xml::$R/var/rootdata/volte/data-profile.xml"
    } > "$R/pps/services/cds/cnice/status_persistent"
}

# Detached, so it is not left as a zombie that still looks alive.
start_daemon() {
    ( sleep 300 >/dev/null 2>&1 & echo $! > "$R/daemon.pid" )
}

snapshot() { (cd "$R" && find var pps accounts | LC_ALL=C sort; cat pps/services/cds/cnice/status_persistent); }

run() { "$SH" -s "$@" < "$R/cleanup.sh" > "$R/out.txt" 2>&1; }

sed "s#/var/#$R/var/#g; s#/accounts/#$R/accounts/#g; s#/pps/#$R/pps/#g; s#/proc/boot/pathtrust#$R/bin/pathtrust#g; s#/usr/bin/python3.2#$R/bin/python3.2#g" \
    "$SCRIPT" > "$R/cleanup.sh"

mkdir "$R/bin"
cat > "$R/bin/pidin" <<EOF
#!/bin/sh
kill -0 "\$(cat "$R/daemon.pid")" 2>/dev/null &&
    echo "\$(cat "$R/daemon.pid") $R/var/addons/volte/init/ims_manager -a enable"
kill -0 "\$(cat "$R/vn-boot.pid" 2>/dev/null)" 2>/dev/null &&
    echo "\$(cat "$R/vn-boot.pid") /bin/sh $R/var/addons/bb10-volte-vn/data/bb10-volte.sh boot"
kill -0 "\$(cat "$R/vn-service.pid" 2>/dev/null)" 2>/dev/null &&
    echo "\$(cat "$R/vn-service.pid") python3.2 $R/var/rootdata/q10_volte/device/service.py start"
echo "99 /bin/not_ims_manager_tool"
exit 0
EOF
cat > "$R/bin/pathtrust" <<EOF
#!/bin/sh
echo "\$1" >> "$R/trusted.txt"
EOF
cat > "$R/bin/python3.2" <<EOF
#!/bin/sh
[ -e "$R/python.fail" ] && { echo "uninstall refused"; exit 1; }
# Proves the uninstall ran while its files and journal still existed.
[ -f "$R/var/rootdata/q10_volte/state/journal.json" ] && echo "\$*" > "$R/uninstalled.txt"
exit 0
EOF
chmod +x "$R/bin/pidin" "$R/bin/pathtrust" "$R/bin/python3.2"
PATH=$R/bin:$PATH
export PATH

# Report mode changes nothing and still names the problems.
make_phone
start_daemon
before=$(snapshot)
run; code=$?
check 'check exits 0' '[ "$code" = 0 ]'
check 'check changes nothing' '[ "$before" = "$(snapshot)" ]'
check 'check leaves the daemon running' 'kill -0 "$(cat "$R/daemon.pid")" 2>/dev/null'
check 'check names the override' 'grep -q "would clear: .*data-profile.xml" "$R/out.txt"'
check 'check shows the old version' 'grep -q "Package-Version: 2.0" "$R/out.txt"'
check 'check lists DCA files for review' 'grep -q "sources/ims/ir92" "$R/out.txt"'

# A wrong option changes nothing.
run aply; code=$?
check 'bad option fails' '[ "$code" != 0 ]'
check 'bad option changes nothing' '[ "$before" = "$(snapshot)" ]'

# No backup means no changes.
chmod a-w "$R/accounts/1000/shared/documents"
run apply; code=$?
chmod u+w "$R/accounts/1000/shared/documents"
check 'failed backup fails' '[ "$code" != 0 ]'
check 'failed backup changes nothing' '[ "$before" = "$(snapshot)" ]'
check 'failed backup leaves the daemon' 'kill -0 "$(cat "$R/daemon.pid")" 2>/dev/null'

# Apply.
run apply; code=$?
copy=$(ls -d "$R"/accounts/1000/shared/documents/volte-cleanup-backup-*)
check 'apply exits 0' '[ "$code" = 0 ]'
check 'daemon stopped' '! kill -0 "$(cat "$R/daemon.pid")" 2>/dev/null'
check 'addon removed' '[ ! -e "$R/var/addons/volte" ]'
check 'renamed copy of the addon removed' '[ ! -e "$R/var/addons/renamed" ]'
check 'rootdata leftover removed' '[ ! -e "$R/var/rootdata/volte" ]'
check 'runtime logs removed' '[ ! -e "$R/var/tmp/bb10-volte" ] && [ ! -e "$R/var/run/bb10_volte_stats.txt" ]'
check 'override cleared' '[ "$(grep "^cdsc_xml::" "$R/pps/services/cds/cnice/status_persistent" | tail -n 1)" = "cdsc_xml::" ]'
check 'other PPS values kept' 'grep -q "^data_service_mode::ON" "$R/pps/services/cds/cnice/status_persistent"'
check 'unrelated addon kept' '[ -f "$R/var/addons/keys/MANIFEST" ]'
check 'addon mentioning IMS kept and flagged' '[ -f "$R/var/addons/ims-tool/init/start.sh" ] && grep -q "ims-tool: kept, but" "$R/out.txt"'
check 'installer kept' '[ -x "$R/var/rootdata/installer" ]'
check 'DCA files kept' '[ -f "$R/var/dca/sources/ims/ir92" ]'
check 'imsconfig files removed' '[ ! -e "$R/var/ims/carrier/imsconfig_182.xml" ] && [ ! -e "$R/var/ims/carrier/imsconfig_-01.xml" ] && [ ! -e "$R/var/ims/carrier/imsconfig_user.xml" ]'
check 'imsconfig files backed up' '[ -f "$copy$R/var/ims/carrier/imsconfig_-01.xml" ] && [ -f "$copy$R/var/ims/carrier/imsconfig_user.xml" ]'
check 'other files in that folder kept' '[ -f "$R/var/ims/carrier/contacts-not-ours" ]'
check 'stock-dated imsconfig kept and flagged' '[ -f "$R/var/ims/carrier/imsconfig_default.xml" ] && grep -q "imsconfig_default.xml: kept" "$R/out.txt"'

# With nothing to compare dates against, a file stays.
make_phone
rm "$R/var/dca/sources/ims/services/carrier_config/ims/emergency"
run apply
copy=$(ls -d "$R"/accounts/1000/shared/documents/volte-cleanup-backup-*)
check 'no date reference keeps imsconfig files' '[ -f "$R/var/ims/carrier/imsconfig_182.xml" ]'
check 'addon backed up' '[ -f "$copy$R/var/addons/volte/data/carrier-182-volte.xml" ]'
check 'override XML backed up' '[ -f "$copy/data-profile.xml" ]'
check 'rootdata backed up' '[ -f "$copy$R/var/rootdata/volte/data-profile.xml" ]'

# A second run finds nothing left to do.
run; code=$?
check 'second check is clean' '[ "$code" = 0 ] && grep -q "^none" "$R/out.txt" && ! grep -q "would" "$R/out.txt"'

# The third-party addon, with its service installed.
make_vn_phone() {
    make_phone
    rm -rf "$R/var/addons/volte" "$R/var/addons/renamed" "$R/var/addons/ims-tool" "$R/var/rootdata/volte"
    rm -f "$R/python.fail" "$R/uninstalled.txt" "$R/trusted.txt"
    mkdir -p "$R/var/addons/bb10-volte-vn/data" "$R/var/addons/bb10-volte-vn/init" \
        "$R/var/addons/q10-unicom-volte/init" "$R/var/rootdata/q10_volte/device" "$R/var/rootdata/q10_volte/state" \
        "$R/var/rootdata/q10_volte_stage_addon123" "$R/var/rootdata/volte-enabler-backup-20261106-233511/addon-volte" \
        "$R/pps/volte_probe" "$R/accounts/1000/shared/downloads"
    : > "$R/var/addons/bb10-volte-vn/data/bb10-volte.sh"
    : > "$R/var/addons/bb10-volte-vn/init/start.sh"
    : > "$R/var/addons/q10-unicom-volte/init/start.sh"
    : > "$R/var/rootdata/q10_volte/installation.json"
    echo '{"restore_completed": false}' > "$R/var/rootdata/q10_volte/state/journal.json"
    : > "$R/var/rootdata/q10_volte/device/install.py"
    : > "$R/var/rootdata/q10_volte/device/run.sh"
    : > "$R/var/rootdata/bb10-volte-addon.log"
    : > "$R/var/rootdata/bb10-volte-cleanup-pending"
    : > "$R/var/rootdata/.volte-enabler-cleaner-voice.py"
    : > "$R/pps/volte_probe/ims"
    : > "$R/accounts/1000/shared/downloads/bb10-volte-boot-trace.txt"
    : > "$R/accounts/1000/shared/downloads/bb10-volte-diag-20261106-233000.txt"
    : > "$R/accounts/1000/shared/downloads/bb10-volte-vn-addon-0.3.3.zip"
    : > "$R/accounts/1000/shared/downloads/keep-me.txt"
}
start_vn_boot() { ( sleep 300 >/dev/null 2>&1 & echo $! > "$R/vn-boot.pid" ); }
start_vn_service() { ( sleep 300 >/dev/null 2>&1 & echo $! > "$R/vn-service.pid" ); }

make_vn_phone
start_vn_boot
before=$(snapshot)
run; code=$?
check 'vn: check exits 0' '[ "$code" = 0 ]'
check 'vn: check changes nothing' '[ "$before" = "$(snapshot)" ]'
check 'vn: check leaves its boot script' 'kill -0 "$(cat "$R/vn-boot.pid")" 2>/dev/null'
check 'vn: check names its uninstall' 'grep -q "would run its uninstall" "$R/out.txt"'
check 'vn: check runs nothing' '[ ! -e "$R/uninstalled.txt" ] && [ ! -e "$R/trusted.txt" ]'

# A failed uninstall keeps the journal and stops the addon from acting at the next boot.
touch "$R/python.fail"
run apply; code=$?
rm -f "$R/python.fail"
check 'vn: failed uninstall fails' '[ "$code" != 0 ] && grep -q "Its uninstall failed" "$R/out.txt"'
check 'vn: failed uninstall keeps the journal' '[ -f "$R/var/rootdata/q10_volte/state/journal.json" ]'
check 'vn: failed uninstall keeps the addon' '[ -f "$R/var/addons/bb10-volte-vn/data/bb10-volte.sh" ]'
check 'vn: failed uninstall blocks auto install' '[ -f "$R/var/addons/bb10-volte-vn/data/no-auto-install" ] && [ ! -e "$R/var/rootdata/bb10-volte-cleanup-pending" ]'
check 'vn: boot script stopped' '! kill -0 "$(cat "$R/vn-boot.pid")" 2>/dev/null'

make_vn_phone
start_vn_boot
run apply; code=$?
copy=$(ls -d "$R"/accounts/1000/shared/documents/volte-cleanup-backup-*)
check 'vn: apply exits 0' '[ "$code" = 0 ]'
check 'vn: boot script stopped first' '! kill -0 "$(cat "$R/vn-boot.pid")" 2>/dev/null'
check 'vn: its uninstall ran before removal' 'grep -q "q10_volte/device/install.py uninstall" "$R/uninstalled.txt"'
check 'vn: its files were trusted first' 'grep -q "!.*q10_volte/device/run.sh" "$R/trusted.txt"'
check 'vn: both addons removed' '[ ! -e "$R/var/addons/bb10-volte-vn" ] && [ ! -e "$R/var/addons/q10-unicom-volte" ]'
check 'vn: service folder removed' '[ ! -e "$R/var/rootdata/q10_volte" ] && [ ! -e "$R/var/rootdata/q10_volte_stage_addon123" ]'
check 'vn: rootdata leftovers removed' '[ ! -e "$R/var/rootdata/bb10-volte-addon.log" ] && [ ! -e "$R/var/rootdata/volte-enabler-backup-20261106-233511" ] && [ ! -e "$R/var/rootdata/.volte-enabler-cleaner-voice.py" ]'
check 'vn: PPS object removed' '[ ! -e "$R/pps/volte_probe" ]'
check 'vn: its reports removed' '[ ! -e "$R/accounts/1000/shared/downloads/bb10-volte-boot-trace.txt" ] && [ ! -e "$R/accounts/1000/shared/downloads/bb10-volte-diag-20261106-233000.txt" ]'
check 'vn: journal backed up' '[ -f "$copy$R/var/rootdata/q10_volte/state/journal.json" ]'
check 'vn: earlier backup of this addon backed up' '[ -d "$copy$R/var/rootdata/volte-enabler-backup-20261106-233511/addon-volte" ]'
check 'vn: unrelated addon kept' '[ -f "$R/var/addons/keys/MANIFEST" ]'
check 'vn: unrelated downloads kept' '[ -f "$R/accounts/1000/shared/downloads/keep-me.txt" ]'
check 'vn: zip kept and listed' '[ -f "$R/accounts/1000/shared/downloads/bb10-volte-vn-addon-0.3.3.zip" ] && grep -q "bb10-volte-vn-addon-0.3.3.zip" "$R/out.txt"'
check 'vn: installer kept' '[ -x "$R/var/rootdata/installer" ]'
run; code=$?
check 'vn: second check is clean' '[ "$code" = 0 ] && ! grep -q "would" "$R/out.txt"'

# Unpacked one level deeper than its guide says: the folder name alone identifies it.
make_vn_phone
rm -rf "$R/var/rootdata/q10_volte" "$R/var/addons/q10-unicom-volte" "$R/var/addons/bb10-volte-vn/data" "$R/var/addons/bb10-volte-vn/init"
mkdir -p "$R/var/addons/bb10-volte-vn/bb10-volte-vn-addon-0.3.3/data"
run apply; code=$?
check 'vn nested: apply exits 0' '[ "$code" = 0 ]'
check 'vn nested: addon removed' '[ ! -e "$R/var/addons/bb10-volte-vn" ]'

# Only the package unzipped, its service never installed: nothing to uninstall.
make_vn_phone
rm -rf "$R/var/rootdata/q10_volte" "$R/var/addons/q10-unicom-volte"
start_vn_service
run apply; code=$?
check 'vn package only: apply exits 0' '[ "$code" = 0 ]'
check 'vn package only: no uninstall' '[ ! -e "$R/uninstalled.txt" ]'
check 'vn package only: stray service stopped' '! kill -0 "$(cat "$R/vn-service.pid")" 2>/dev/null'
check 'vn package only: addon removed' '[ ! -e "$R/var/addons/bb10-volte-vn" ]'

rm -rf "$R"
[ "$FAILS" = 0 ] && echo "All passed." || { echo "$FAILS failed."; exit 1; }
