/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_ARM_SUSPEND_H__
#define __ASM_ARM_SUSPEND_H__

int32_t domain_suspend(register_t epoint, register_t cid);
void hyp_resume(void);

#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
