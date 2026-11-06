#!/bin/sh
# Runs volte-update.sh against a fake phone in a temp folder. No phone needed.
#   sh tests/test_update.sh       or: SH=ksh sh tests/test_update.sh

SH=${SH:-sh}
SCRIPT=$(dirname "$0")/../volte-update.sh
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
    rm -rf "$R/var" "$R/accounts"
    mkdir -p "$R/var/addons/volte/init" "$R/var/rootdata" "$R/var/tmp" \
        "$R/accounts/1000/shared/downloads" "$R/accounts/1000/shared/documents"
    echo 'Package-Version: old' > "$R/var/addons/volte/MANIFEST"
    : > "$R/var/addons/volte/init/ims_manager"
    touch -t 202601010000 "$R/var/addons/volte/init/ims_manager"
    : > "$R/accounts/1000/shared/downloads/volte.zip"
    touch -t 202601010000 "$R/accounts/1000/shared/downloads/volte.zip"
    # Newer than the real zip, so it would win if it weren't excluded.
    : > "$R/accounts/1000/shared/documents/volte-debug.zip"

    # Like the real installer, it updates the MANIFEST and only replaces the binary if nothing is
    # running it. Reading stdin here would eat the rest of a script piped into sh.
    cat > "$R/var/rootdata/installer" <<EOF
#!/bin/sh
cat > /dev/null
echo "\$1" > "$R/installer-arg"
echo 'Package-Version: new' > "$R/var/addons/volte/MANIFEST"
[ -f "$R/stuck-binary" ] || kill -0 "\$(cat "$R/daemon.pid")" 2>/dev/null ||
    : > "$R/var/addons/volte/init/ims_manager"
echo 'Installation completed successfully.'
EOF
    chmod +x "$R/var/rootdata/installer"
    rm -f "$R/installer-arg" "$R/stuck-binary"
}

# Detached, so it is not left as a zombie that still looks alive.
start_daemon() {
    ( sleep 300 >/dev/null 2>&1 & echo $! > "$R/daemon.pid" )
}

run() { "$SH" -s "$@" < "$R/update.sh" > "$R/out.txt" 2>&1; }

sed "s#/var/#$R/var/#g; s#/accounts/#$R/accounts/#g" "$SCRIPT" > "$R/update.sh"

mkdir "$R/bin"
cat > "$R/bin/pidin" <<EOF
#!/bin/sh
kill -0 "\$(cat "$R/daemon.pid")" 2>/dev/null &&
    echo "\$(cat "$R/daemon.pid") $R/var/addons/volte/init/ims_manager -a enable"
echo "99 /bin/not_ims_manager_tool"
exit 0
EOF
chmod +x "$R/bin/pidin"
PATH=$R/bin:$PATH

echo "--- daemon running"
make_phone
start_daemon
run; rc=$?
check "exits 0" '[ "$rc" = 0 ]'
check "daemon stopped" '! kill -0 "$(cat "$R/daemon.pid")" 2>/dev/null'
check "says success" 'grep -q "^SUCCESS" "$R/out.txt"'
check "script survives an installer reading stdin" 'grep -q "Package-Version: new" "$R/out.txt"'
check "picks the zip, not a newer debug bundle" 'grep -q "/downloads/volte.zip" "$R/installer-arg"'
check "removes its stamp" '[ ! -e "$R/var/tmp/volte-update.stamp" ]'

echo "--- installer claims success, binary unchanged"
make_phone
touch "$R/stuck-binary"
run; rc=$?
check "exits 1" '[ "$rc" = 1 ]'
check "says failed" 'grep -q "^FAILED" "$R/out.txt"'
check "does not say success" '! grep -q "^SUCCESS" "$R/out.txt"'
check "removes its stamp" '[ ! -e "$R/var/tmp/volte-update.stamp" ]'

echo "--- no zip"
make_phone
rm "$R/accounts/1000/shared/downloads/volte.zip"
start_daemon
run; rc=$?
check "exits 1" '[ "$rc" = 1 ]'
check "leaves the daemon running" 'kill -0 "$(cat "$R/daemon.pid")" 2>/dev/null'
check "does not run the installer" '[ ! -e "$R/installer-arg" ]'
kill "$(cat "$R/daemon.pid")"

echo "--- zip given by hand"
make_phone
: > "$R/my zip.zip"
run "$R/my zip.zip"; rc=$?
check "exits 0" '[ "$rc" = 0 ]'
check "uses that zip" 'grep -q "my zip.zip" "$R/installer-arg"'

rm -rf "$R"
[ "$FAILS" = 0 ] && echo "all passed" || { echo "$FAILS failed"; exit 1; }
