#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

base=$(git rev-parse --verify --end-of-options "${1:?Missing VTIMER_BASE_REV}^{commit}")
tip=$(git rev-parse HEAD)
if [[ "$base" == "$tip" ]] || ! git merge-base --is-ancestor "$base" "$tip"; then
    echo "VTIMER_BASE_REV must be an ancestor of the pipeline commit" >&2
    exit 1
fi

work=$(mktemp -d "$PWD/build-vtimer-before.XXXXXX")
trap 'rm -r -- "$work"' EXIT
git archive "$base" | tar -x -C "$work"
git show -s --format='%ad git:%h' "$base" > "$work/.scmversion"

test -s binaries/xen
grep -qx 'CONFIG_DEBUG=y' xen-config
grep -qx 'CONFIG_PERF_COUNTERS=y' xen-config
cp xen-config "$work/xen/.config"
make -C "$work/xen" olddefconfig
cmp xen-config "$work/xen/.config"
bash "$(dirname "$0")/prepare-handler.sh" "$work" "$work/xen/arch/arm" before
make -j"$(nproc)" -C "$work/xen"
cp "$work/xen/xen" binaries/xen-before
printf 'before %s\nafter %s\n' "$base" "$tip" | tee binaries/vtimer-revisions
