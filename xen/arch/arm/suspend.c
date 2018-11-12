/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/sched.h>
#include <asm/cpufeature.h>
#include <asm/event.h>
#include <asm/psci.h>
#include <asm/suspend.h>
#include <asm/platform.h>
#include <public/sched.h>

static void vcpu_suspend_prepare(register_t epoint, register_t cid)
{
    struct vcpu *v = current;

    v->arch.suspend_ep = epoint;
    v->arch.suspend_cid = cid;
}

int32_t domain_suspend(register_t epoint, register_t cid)
{
    struct vcpu *v;
    struct domain *d = current->domain;
    bool is_thumb = epoint & 1;

    dprintk(XENLOG_DEBUG,
            "Dom%d suspend: epoint=0x%"PRIregister", cid=0x%"PRIregister"\n",
            d->domain_id, epoint, cid);

    /* THUMB set is not allowed with 64-bit domain */
    if ( is_64bit_domain(d) && is_thumb )
        return PSCI_INVALID_ADDRESS;

    /* TODO: care about locking here */
    /* Ensure that all CPUs other than the calling one are offline */
    for_each_vcpu ( d, v )
    {
        if ( v != current && is_vcpu_online(v) )
            return PSCI_DENIED;
    }

    /*
     * Prepare the calling VCPU for suspend (save entry point into pc and
     * context ID into r0/x0 as specified by PSCI SYSTEM_SUSPEND)
     */
    vcpu_suspend_prepare(epoint, cid);

    /* Disable watchdogs of this domain */
    watchdog_domain_suspend(d);

    /*
     * The calling domain is suspended by blocking its last running VCPU. If an
     * event is pending the domain will resume right away (VCPU will not block,
     * but when scheduled in it will resume from the given entry point).
     */
    vcpu_block_unless_event_pending(current);

    return PSCI_SUCCESS;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
