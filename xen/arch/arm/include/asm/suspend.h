/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ARM_SUSPEND_H
#define ARM_SUSPEND_H

struct domain;
struct vcpu;

struct resume_info {
    register_t ep;
    register_t cid;
    struct vcpu *wake_cpu;
};

int arch_domain_resume(struct domain *d);

#endif /* ARM_SUSPEND_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
