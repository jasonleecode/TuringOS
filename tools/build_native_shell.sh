#!/usr/bin/env bash
exec "$(dirname "$0")/../build.sh" --board "${1:-virt}" --pkg native_shell
