#!/bin/sh
# Prints the raw SDP e= (email) lines the network sent in call replies, unscrubbed.
# Only those lines are kept: nothing else from the logs is collected.
# Run as root right after a failed outgoing VoLTE call (the log rotates fast):
#   cat volte-email.sh | sh

OUT=/accounts/1000/shared/documents/volte-email.txt

slog2info -b ims_rrm 2>&1 | grep -E 'sip +3261 +e=|Could not parse email' > "$OUT"
chmod g+rw "$OUT"

cat "$OUT"
[ -s "$OUT" ] || echo "No e= line found: make a call first, then run this at once."
