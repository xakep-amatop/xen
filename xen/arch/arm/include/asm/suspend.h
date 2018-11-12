/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ARM_SUSPEND_H__
#define __ARM_SUSPEND_H__

struct resume_info {
    register_t ep;
    register_t cid;
    struct vcpu *wake_cpu;
};

int arch_domain_resume(struct domain *d);

#endif /* __ARM_SUSPEND_H__ */

 /*
  * Local variables:
  * mode: C
  * c-file-style: "BSD"
  * c-basic-offset: 4
  * tab-width: 4
  * indent-tabs-mode: nil
  * End:
  */
