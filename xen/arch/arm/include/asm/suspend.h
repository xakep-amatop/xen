/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_ARM_SUSPEND_H__
#define __ASM_ARM_SUSPEND_H__

#include <asm/types.h>

#ifdef CONFIG_SYSTEM_SUSPEND

#ifdef CONFIG_ARM_64
struct cpu_context {
    register_t callee_regs[12];
    register_t sp;
    register_t vbar_el2;
    register_t vtcr_el2;
    register_t vttbr_el2;
    register_t tpidr_el2;
    register_t mdcr_el2;
    register_t hstr_el2;
    register_t cptr_el2;
    register_t hcr_el2;
} __aligned(16);
#else
#error "Define cpu_context structure for arm32"
#endif

extern struct cpu_context cpu_context;

void hyp_resume(void);
int prepare_resume_ctx(struct cpu_context *ptr);

int host_system_suspend(void);

#endif /* CONFIG_SYSTEM_SUSPEND */

#endif /* __ASM_ARM_SUSPEND_H__ */

 /*
  * Local variables:
  * mode: C
  * c-file-style: "BSD"
  * c-basic-offset: 4
  * tab-width: 4
  * indent-tabs-mode: nil
  * End:
  */
