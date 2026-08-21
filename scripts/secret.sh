#!/usr/bin/env bash
#
# Emit a single -D build flag for one key read from the secrets file.
#
#   usage: secret.sh KEY [--raw]
#
# Values are quoted as C string literals by default; --raw emits the value
# bare, for numeric defines such as MQTT_PORT.
#
# A missing or empty key aborts the build. Previously a missing key silently
# produced -D KEY="" and the firmware shipped with blank credentials, which
# then looked like a runtime WiFi or broker fault.
set -euo pipefail

key="${1:?usage: secret.sh KEY [--raw]}"
mode="${2:-}"
file="${SECRETS_FILE:-../.secrets}"

die() { echo "secret.sh: $*" >&2; exit 1; }

[ -f "$file" ] || die "secrets file not found: $file"

# First field is the key, second the value; anything else on the line is ignored.
value="$(awk -v k="$key" '$1 == k { print $2; exit }' "$file")"
[ -n "$value" ] || die "$key is missing or empty in $file"

if [ "$mode" = "--raw" ]; then
    printf -- "'-D %s=%s'\n" "$key" "$value"
else
    printf -- "'-D %s=\"%s\"'\n" "$key" "$value"
fi
