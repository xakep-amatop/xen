/*
 * Kernel image loading.
 *
 * Copyright (C) 2011 Citrix Systems, Inc.
 */
#ifndef __ARCH_ARM_KERNEL_H__
#define __ARCH_ARM_KERNEL_H__

#include <xen/types.h>

#include <asm/domain.h>

struct kernel_info;

struct arch_kernel_info
{
    /* Enable pl011 emulation */
    bool vpl011;
};

#define arch_get_minimum_first_bank_size arch_get_minimum_first_bank_size
paddr_t arch_get_minimum_first_bank_size(struct kernel_info *info,
                                         paddr_t bank_start);

#endif /* #ifdef __ARCH_ARM_KERNEL_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
