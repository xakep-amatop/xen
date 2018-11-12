/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __XEN_SUSPEND_H__
#define __XEN_SUSPEND_H__

#if __has_include(<asm/suspend.h>)
#include <asm/suspend.h>
#else
static inline int arch_domain_resume(struct domain *d)
{
    return 0;
}
#endif


#endif /* __XEN_SUSPEND_H__ */
