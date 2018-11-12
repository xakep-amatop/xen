/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_ARM_SUSPEND_H__
#define __ASM_ARM_SUSPEND_H__

#ifdef CONFIG_SYSTEM_SUSPEND

int host_system_suspend(void);

void hyp_resume(void);

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
