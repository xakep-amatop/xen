#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

# Read the sources used by this build. The same extraction serves the host test.
arm=${1:?source tree}/xen/arch/arm
out=${2:?output directory}
sed -n '/^#define CNTx_CTL_/p' "$arm/include/asm/processor.h" > "$out/vtimer-handler.inc"
sed -n '/^static void vtimer_interrupt(/,/^}/p' "$arm/time.c" >> "$out/vtimer-handler.inc"

if [[ $# -eq 3 ]]; then
    case $3 in
        before) old=1 ;;
        after)  old=0 ;;
        *) exit 2 ;;
    esac
    {
        echo "#define VTIMER_TEST_BEFORE $old"
        cat "$(dirname "$0")/check-handler.c"
    } > "$out/vtimer-selftest.c"
    rule='obj-y += vtimer-selftest.o'
    grep -Fqx "$rule" "$out/Makefile" || echo "$rule" >> "$out/Makefile"
fi
