#!/usr/bin/env bash
exec "$(dirname "$0")/../build.sh" --board "${2:-virt}" --pkg "${1:?specify an L4Re package under pkg/}"
