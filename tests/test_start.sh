#!/bin/sh
# Runs pkg/init/start.sh against a fake phone in a temp folder. No phone needed.
#   sh tests/test_start.sh       or: SH=ksh sh tests/test_start.sh

SH=${SH:-sh}
SCRIPT=$(dirname "$0")/../pkg/init/start.sh
R=$(mktemp -d)
FAILS=0
INIT=$R/addon/init

check() {
    if eval "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        FAILS=$((FAILS + 1))
    fi
}

mkdir -p "$INIT" "$R/bin"
sed "s#/var/log/#$R/#; s#/proc/boot/pathtrust#$R/bin/pathtrust#" "$SCRIPT" > "$INIT/start.sh"
echo 'Package-Version: new' > "$R/addon/MANIFEST"

# A stub that ignores SIGTERM stands in for a daemon that won't stop.
cat > "$R/bin/pidin" <<EOF
#!/bin/sh
kill -0 "\$(cat "$R/daemon.pid")" 2>/dev/null && echo "\$(cat "$R/daemon.pid") $INIT/ims_manager -a enable"
echo "99 /bin/not_ims_manager_tool"
EOF
printf '#!/bin/sh\necho "$@" > %s/on-args\n' "$R" > "$R/bin/on"
printf '#!/bin/sh\necho "$1" > %s/trusted\n' "$R" > "$R/bin/pathtrust"
chmod +x "$R/bin/"*
PATH=$R/bin:$PATH

reset() {
    rm -f "$R/on-args" "$R/trusted" "$R/installer.log" "$R/daemon.pid"
    echo old > "$INIT/ims_manager"
    echo new > "$INIT/ims_manager.new"
}

start_daemon() {
    ( sleep 300 >/dev/null 2>&1 & echo $! > "$R/daemon.pid" )
}

echo "--- update with the daemon running"
reset
start_daemon
"$SH" "$INIT/start.sh"; rc=$?
check "exits 0" '[ "$rc" = 0 ]'
check "old daemon stopped" '! kill -0 "$(cat "$R/daemon.pid")" 2>/dev/null'
check "new binary in place" 'grep -q new "$INIT/ims_manager"'
check ".new removed" '[ ! -e "$INIT/ims_manager.new" ]'
check "trusted" 'grep -qx "!$INIT/ims_manager" "$R/trusted"'
check "logs the version" 'grep -q "UPDATED: Package-Version: new" "$R/installer.log"'
check "starts the daemon" 'grep -q "$INIT/ims_manager -a enable" "$R/on-args"'

echo "--- boot, nothing to update"
reset
rm "$INIT/ims_manager.new"
"$SH" "$INIT/start.sh"; rc=$?
check "exits 0" '[ "$rc" = 0 ]'
check "binary untouched" 'grep -q old "$INIT/ims_manager"'
check "trusted" 'grep -qx "!$INIT/ims_manager" "$R/trusted"'
check "starts the daemon" '[ -e "$R/on-args" ]'

rm -rf "$R"
[ "$FAILS" = 0 ] && echo "all passed" || { echo "$FAILS failed"; exit 1; }
