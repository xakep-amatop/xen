#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

app=$(cd -- "$(dirname -- "$0")/zephyr" && pwd)
work=$(mktemp -d "$PWD/build-zephyr-vtimer.XXXXXX")
trap 'rm -r -- "$work"' EXIT
out="$PWD/binaries/zephyr"

fetch() {
    local dir="$work/$1" rev=$2
    git init --quiet "$dir"
    git -C "$dir" fetch --depth=1 --no-tags "https://github.com/zephyrproject-rtos/$1.git" "$rev"
    git -C "$dir" checkout --quiet --detach FETCH_HEAD
    test "$(git -C "$dir" rev-parse HEAD)" = "$rev"
}

fetch zephyr 41d10b59730406bbab4ca9f1fbbe9e96aafb4966
# Xen headers from this release's west.yml.
fetch zephyr-xenlib 8b13d0b709314b7de6cb5877c286d52e71095cc8

cc=$(command -v aarch64-linux-gnu-gcc)
export CROSS_COMPILE=${cc%gcc} ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export ZEPHYR_BASE="$work/zephyr"
for mode in periodic tickless; do
    build="$work/$mode"
    tickless=n
    if [ "$mode" = tickless ]; then tickless=y; fi
    echo "CONFIG_TICKLESS_KERNEL=$tickless" > "$work/mode.conf"
    cmake -GNinja -S "$app" -B "$build" -DBOARD=xenvm/xenvm/gicv3 \
        -DZEPHYR_MODULES="$work/zephyr-xenlib" \
        -DEXTRA_CONF_FILE="$app/common.conf;$work/mode.conf" \
        -DDTC_OVERLAY_FILE="$app/consoleio.overlay" \
        -DPython3_EXECUTABLE="$(command -v python3)" -DUSER_CACHE_DIR="$work/cache" \
        -DEXTRA_CFLAGS=-mno-outline-atomics
    cmake --build "$build" --parallel 4
    mkdir -p "$out/$mode"
    cp "$build/zephyr/zephyr.bin" "$build/zephyr/.config" "$out/$mode/"
done
(cd "$out"; sha256sum {periodic,tickless}/{zephyr.bin,.config} > SHA256SUMS)
