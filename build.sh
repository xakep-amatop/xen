#!/bin/bash

export CROSS_COMPILE=aarch64-linux-gnu-

make distclean

make -j$(nproc) dist-xen XEN_TARGET_ARCH=arm64

mkimage -A arm64 -C none -T kernel -a 0x9a080000 -e 0x9a080000 -n "XEN" -d xen/xen xen-uImage
# scp xen-uImage mykola_kvach@192.168.198.182:/media/mykola_kvach/data_ext/tftpboot/xen/xen
cp xen-uImage /media/mykola_kvach/data_ext/tftpboot/xen/xen

exit 0
