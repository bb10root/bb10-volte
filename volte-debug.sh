#!/bin/sh
# Collects VoLTE diagnostics into one folder. Writes nothing else on the phone.
# Run as root, soon after the failure and BEFORE rebooting: a reboot can wipe /var/tmp.
#   cat volte-debug.sh | sh
#   (not "sh volte-debug.sh": the shell refuses script files on untrusted paths)
#   Another output folder: cat volte-debug.sh | sh -s /path/to/folder
#
# Only named logs and fields are kept, then scrubbed of personal data: Wi-Fi, location,
# messages and other apps' logs are never collected. IP addresses stay, for route debugging.

OUT=${1:-/accounts/1000/shared/documents/volte-debug}
mkdir -p "$OUT" || { echo "Cannot create $OUT"; exit 1; }

# The phone's sed has -r but not -E. Avoid single quotes inside these patterns.
scrub() {
    sed -r \
        -e 's/(Authorization|WWW-Authenticate|Proxy-Authenticate|Authentication-Info)[ ]*:.*/\1: <hidden>/Ig' \
        -e 's/\b(nonce|cnonce|response|opaque|auts|rand|autn|ck|ik|res|kc)[ ]*=[ ]*("[^"]*"|[^ ,;]*)/\1=<hidden>/Ig' \
        -e 's/\b(username|password|passwd|pwd|secret|token)"?[ ]*[=:][ ]*("[^"]*"|[^ ,;]*)/\1=<hidden>/Ig' \
        -e 's/\b(imsi|imei|imeisv|meid|iccid|msisdn|impi|impu|mdn)\b"?[ ]*[=:[][ ]*"?[^ ,;"<>]+/\1=<hidden>/Ig' \
        -e 's/\b(utran-cell-id-3gpp|cell_?id|cell_identity|lac|tac)"?[ ]*[=:][ ]*"?[0-9a-fx]+/\1=<hidden>/Ig' \
        -e 's/(urn:gsma:imei:|urn:uuid:)[^ ;">]+/\1<hidden>/Ig' \
        -e 's/"[^"]*"[ ]*<(sips?|tel):/"<hidden>" <\1:/Ig' \
        -e 's/\btel:[^ ;">]+/tel:<hidden>/Ig' \
        -e 's/[+0-9]*\*{3,}[0-9]*/<hidden>/g' \
        -e 's/[A-Za-z0-9._%+*-]+@/<hidden>@/g' \
        -e 's/\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b/<uuid>/g' \
        -e 's/\b[0-9A-Fa-f]{16,}\b/<hex>/g' \
        -e 's/[0-9]{8}-[0-9]{6}-[0-9]{1,2}/<num>/g' \
        -e 's/\+?[0-9]{9,}/<num>/g' \
        -e 's/\+[0-9]{6,}/<num>/g'
}

# Both always overwrite, so a source that has gone missing can't leave an old copy behind.
save() { cat "$1" 2>&1 | scrub > "$OUT/$2"; }

# Keeps only the named PPS attributes.
save_fields() {
    cat "$1" 2>&1 | grep -E "^(\[n\])?($2):|No such file|Permission denied" | scrub > "$OUT/$3"
}

echo "Collecting into $OUT ..."

# This log is shared with other addons, so keep only this addon's lines.
cat /var/log/installer.log 2>&1 |
    grep -E '\[BBE\]|/addons/volte|did not exit|Installation completed|No such file' |
    scrub > "$OUT/installer.log"

save /var/run/bb10_volte_stats.txt stats.txt

for name in provider ims-start dca-enable route; do
    save "/var/tmp/bb10-volte/$name.log" "$name.log"
done

save_fields /pps/services/ims/status_public \
    'SMS_Format|WiFiCalling_enabled|allowVoLteProvisioning|default_ran|default_sys|deviceConfig|profiles|rcse_local_caps|reason|roaming|status|version' \
    pps-ims-status_public.txt
save_fields /pps/services/cellular/radioctrl/status_public \
    'active_bands|band_capability|cell_search_status|network_service|network_technology|power_status|provider_capability|radio_device_info' \
    pps-radioctrl-status_public.txt
save_fields /pps/services/cellular/uicc/card0/status_private \
    'uicc_state|uicc_type|sap_state|mep_lock_state|hplmn|gid1' \
    pps-uicc-status_private.txt
save_fields /pps/services/carriermanager/status_private \
    'true_home_carrier|defaultPreloadedConfigFile' \
    pps-carriermanager-status_private.txt
save_fields /pps/services/cds/cnice/status_persistent \
    'data_service_mode|data_roaming_mode|autodata|cdsc_xml|payment_profile_data_signature|payment_profile_determination_type' \
    pps-cnice-status_persistent.txt
save_fields /pps/services/phone/public/status \
    'phone_state|phone_active' \
    pps-phone-status.txt

{
    date
    uname -a
    echo "--- processes"
    pidin arg | grep -E '[i]ms_manager|[i]ms_rrm'
    echo "--- /var/tmp/bb10-volte"
    ls -l /var/tmp/bb10-volte
} 2>&1 | scrub > "$OUT/system.txt"

# The addon's folder is only known from the running daemon's path.
daemon=
for word in $(pidin arg 2>/dev/null | grep '[i]ms_manager'); do
    case $word in */ims_manager) daemon=$word ;; esac
done

if [ -n "$daemon" ]; then
    addon=${daemon%/*}/..
    {
        ls -l "$daemon"
        echo "--- MANIFEST"
        cat "$addon/MANIFEST"
        echo "--- data"
        ls -l "$addon/data"
    } > "$OUT/addon.txt" 2>&1
    for xml in "$addon"/data/carrier-auto-*-volte.xml; do
        [ -f "$xml" ] && save "$xml" "${xml##*/}"
    done
else
    echo "ims_manager not found: it is not running, or this was not run as root." > "$OUT/addon.txt"
fi

slog2info -b bb10_volte,ims_rrm,ims_rrm_if,qct_rrm,cellular_control,carrier_mgr 2>&1 |
    scrub > "$OUT/slog2info.txt"

# Files are owned by root; this lets the normal user read and delete them.
chmod -R g+rwX "$OUT"

echo "Done. Personal data is filtered out, but the filter can miss things:"
echo "skim the files before sharing them."
