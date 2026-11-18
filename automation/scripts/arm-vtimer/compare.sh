#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(git rev-parse --show-toplevel)
base=$(git rev-parse --verify --end-of-options "${1:?Usage: compare.sh BASE [TIP]}^{commit}")
tip=$(git rev-parse --verify --end-of-options "${2:-HEAD}^{commit}")
git merge-base --is-ancestor "$base" "$tip" || { echo 'BASE must be an ancestor of TIP' >&2; exit 1; }

work=$(mktemp -d "$PWD/build-vtimer-compare.XXXXXX")
trap 'rm -r -- "$work"' EXIT

check() {
    local side=$1 rev=$2
    echo "$side $rev"
    git -C "$root" archive "$rev" xen/arch/arm/{time.c,include/asm/processor.h} | tar -x -C "$work"
    bash "$here/prepare-handler.sh" "$work" "$work"
    "${HOSTCC:-cc}" -std=c11 -Wall -Werror -I "$work" "$here/check-handler.c" -o "$work/check"
    "$work/check" "$side"
}

check before "$base"
check after "$tip"
