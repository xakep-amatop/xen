#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd)
usage() { echo 'Usage: run.sh linux {before-selftest|after-selftest|before-control|after-control|after-irq} | zephyr {periodic|tickless}' >&2; exit 2; }
[ "$#" -eq 2 ] || usage
guest=$1
variant=$2
name=$guest-$variant
xen_image=xen
selftest=
case "$guest" in
    linux)
        memory=2048; limit=60
        case "$variant" in
            before-control) xen_image=xen-before; irqs=0 ;;
            after-control) irqs=0 ;;
            before-selftest) xen_image=xen-before; irqs=0; selftest=before ;;
            after-selftest) irqs=0; selftest=after ;;
            after-irq) irqs=1 ;;
            *) usage ;;
        esac ;;
    zephyr)
        [[ $variant == periodic || $variant == tickless ]] || usage
        memory=1024; limit=30; irqs=40 ;;
    *) usage ;;
esac
out=$(realpath -m "${OUT:-vtimer-results}")
mkdir "$out"

run_case() (
    set -e
    qemu=$(command -v "${QEMU:-qemu-system-aarch64}")
    binaries=$(realpath "${BINARIES:-binaries}")
    xen=$binaries/$xen_image
    cat "$binaries/vtimer-revisions"
    echo "Testing $xen_image for $name"
    "$qemu" --version > "$out/qemu-version"
    sha256sum "$xen" > "$out/SHA256SUMS"
    printf '%s\n' "case=$name" "GIC=3" "IRQS=$irqs" > "$out/parameters"

    if [ "$guest" = linux ]; then
        linux=$(realpath "${LINUX:-$binaries/Image}")
        rootfs=$(realpath "${ROOTFS:-$binaries/rootfs.cpio.gz}")
        mkdir "$out/initrd"
        cp "$here/linux-init" "$out/initrd/init"
        chmod +x "$out/initrd/init"
        touch -d @0 "$out/initrd/init"
        cp "$rootfs" "$out/initrd.cpio.gz"
        (cd "$out/initrd"; printf 'init\n' | cpio --reproducible -o -H newc --owner=0:0 | gzip -n) \
            >> "$out/initrd.cpio.gz"
        sha256sum "$linux" "$rootfs" "$out/initrd.cpio.gz" >> "$out/SHA256SUMS"
    else
        (cd "$binaries/zephyr"; sha256sum -c SHA256SUMS)
        cp "$binaries/zephyr/$variant/zephyr.bin" "$out/guest.bin"
        cp "$binaries/zephyr/$variant/.config" "$out/guest.config"
    fi

    cd "$out"
    # TEST_TIMEOUT_OVERRIDE is also used by Xen's other console tests.
    limit=${TEST_TIMEOUT_OVERRIDE:-$limit}
    command=("$qemu" -machine virt,virtualization=on,gic-version=3
             -accel tcg -cpu cortex-a57 -m "$memory" -smp 2
             -display none -nodefaults)
    "${command[@]}" -machine dumpdtb=virt.dtb
    put() { fdtput -p -t "$3" virt.dtb "$1" "$2" "${@:4}"; }
    put /pl061@9030000 status s disabled
    put /chosen '#address-cells' x 2
    put /chosen '#size-cells' x 2
    bootargs='console=dtuart dtuart=/pl011@9000000 loglvl=all guest_loglvl=all'
    if [ "$guest" = linux ]; then
        bootargs+=' dom0_mem=512M dom0_max_vcpus=1 dom0_vcpus_pin=true'
        put /chosen xen,dom0-bootargs s 'console=hvc0 root=/dev/ram0 rdinit=/init loglevel=5'
        for type in kernel ramdisk; do
            image=$linux; address=50000000; compatible=xen,linux-zimage
            [ "$type" != ramdisk ] || {
                image=$out/initrd.cpio.gz; address=58000000; compatible=xen,linux-initrd
            }
            node=/chosen/module@$address
            put "$node" compatible s "$compatible" xen,multiboot-module
            put "$node" reg x 0 "$address" 0 "$(printf '%x' "$(stat -c %s "$image")")"
            command+=(-device "loader,file=$image,addr=0x$address")
        done
    else
        grep -qx 'CONFIG_XEN_DOM0LESS=y' guest.config
        grep -qx 'CONFIG_GIC_VER=3' guest.config
        if [ "$variant" = tickless ]; then
            grep -qx 'CONFIG_TICKLESS_KERNEL=y' guest.config
        else
            grep -qx '# CONFIG_TICKLESS_KERNEL is not set' guest.config
        fi
        node=/chosen/domU1
        put "$node" compatible s xen,domain
        put "$node" '#address-cells' x 2
        put "$node" '#size-cells' x 2
        put "$node" memory x 0 10000
        put "$node" cpus x 1
        put "$node" xen,enhanced s no-xenstore
        put "$node/module@50000000" compatible s multiboot,kernel multiboot,module
        put "$node/module@50000000" reg x 0 50000000 0 "$(printf '%x' "$(stat -c %s guest.bin)")"
        command+=(-device "loader,file=$PWD/guest.bin,addr=0x50000000")
        sha256sum guest.bin guest.config >> SHA256SUMS
    fi
    put /chosen bootargs s "$bootargs"
    read -ra timer <<< "$(fdtget -t x virt.dtb /timer interrupts)"
    read -ra regs <<< "$(fdtget -t x virt.dtb /intc@8000000 reg)"
    [[ ${#timer[@]} -eq 12 && ${timer[6]} = 1 ]]
    [[ ${#regs[@]} -ge 8 && ${regs[0]} = 0 && ${regs[4]} = 0 ]]
    ppi=$((16#${timer[7]} + 16))
    [[ $ppi -ge 16 && $ppi -lt 32 ]]
    pending=$((16#${regs[5]} + 0x10200))
    command+=(-no-reboot -monitor none -serial stdio -kernel "$xen" -dtb virt.dtb
              -qtest pipe:qtest -qtest-log qtest.log)
    printf '%q ' "${command[@]}" > command.sh
    printf '\n' >> command.sh
    mkfifo qtest.in qtest.out
    control_rc=0
    timeout -k 5 "$((limit + 10))" expect "$here/console.exp" "$guest" "$irqs" \
        "$limit" "$pending" "$((1 << ppi))" "$selftest" "${command[@]}" || control_rc=$?
    awk -v name="$name" -v guest="$guest" -v requests="$irqs" -v selftest="$selftest" -v control_status="$control_rc" \
        -f "$here/check.awk" console.log > junit.xml
)

set +e
run_case > "$out/run.log" 2>&1
rc=$?
set -e
cat "$out/run.log"
if [ ! -s "$out/junit.xml" ]; then
    if [[ "$rc" == 0 || "$rc" == 77 ]]; then rc=1; fi
    cat > "$out/junit.xml" <<EOF
<testsuites><testsuite name="$name">
<testcase name="run"><failure>See run.log</failure></testcase>
</testsuite></testsuites>
EOF
fi
state=PASS
[ "$rc" = 0 ] || state=FAIL
[ "$rc" != 77 ] || state=INCONCLUSIVE
printf '%s\t%s\n' "$name" "$state" | tee "$out/results.tsv"
rm -f "$out/qtest.in" "$out/qtest.out"
if [ -n "${CI_PIPELINE_URL:-}" ]; then echo "$CI_PIPELINE_URL"; fi
exit "$rc"
