#!/usr/bin/env bash
# AM335x has no matching QEMU machine. Check the deployable ELF.
exec "$(dirname "$0")/../run_qemu.sh" --board bbb --check "$@"
