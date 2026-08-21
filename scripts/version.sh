#!/usr/bin/env bash
#
# Emit a -D flag carrying the current git description.
#
# The firmware prints this at boot, which is the only practical way to tell what
# is running on a screen hanging on a wall - especially once it is being updated
# over the air rather than over a cable you can see.
set -euo pipefail

version="$(git describe --always --dirty --tags 2>/dev/null || echo unknown)"
printf -- "'-D FIRMWARE_VERSION=\"%s\"'\n" "$version"
