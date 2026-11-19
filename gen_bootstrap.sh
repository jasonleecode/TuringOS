#!/usr/bin/env bash
exec "$(dirname "$0")/build.sh" --board "${BOARD:-rpi4}" bootstrap "$@"
