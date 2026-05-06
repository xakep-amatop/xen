/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/psci.h>
#include <asm/suspend.h>

#include <public/sched.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/serial.h>

struct resume_cpu_context resume_cpu_context;

/*
 * Non-PSCI infrastructure can make host suspend impossible before accepting
 * a virtual SYSTEM_SUSPEND request, e.g. when a required driver has no valid
 * suspend/resume path.
 * Keep this gate side-effect-free so vPSCI can use it for PSCI_FEATURES.
 */
static bool host_system_suspend_runtime_allowed = true;

static bool host_serial_suspend_allowed(void)
{
    if ( serial_suspend_supported() )
        return true;

    printk_once(XENLOG_INFO
                "Host SYSTEM_SUSPEND blocked: serial driver lacks suspend/resume support\n");

    return false;
}

bool host_system_suspend_allowed(void)
{
    return psci_system_suspend_allowed() &&
           host_serial_suspend_allowed() &&
           host_system_suspend_runtime_allowed;
}

void host_system_suspend_disable(const char *reason)
{
    host_system_suspend_runtime_allowed = false;

    printk(XENLOG_INFO "Host SYSTEM_SUSPEND blocked: %s\n",
           reason ? reason : "unsupported suspend/resume path");
}

/*
 * If skip is provided, it is the domain about to be parked by the caller, so
 * don't require it to be in SHUTDOWN_suspend yet.
 */
bool host_system_suspend_domains_ready(const struct domain *skip)
{
    struct domain *d;

    rcu_read_lock(&domlist_read_lock);

    for_each_domain ( d )
    {
        bool domain_suspended;

        if ( d == skip )
            continue;

        spin_lock(&d->shutdown_lock);
        domain_suspended = d->is_shut_down &&
                           d->shutdown_code == SHUTDOWN_suspend;
        spin_unlock(&d->shutdown_lock);

        if ( domain_suspended )
            continue;

        printk(XENLOG_ERR
               "System suspend requires all domains to be shut down for suspend (dom%u: isn't in suspend state)\n",
               d->domain_id);

        rcu_read_unlock(&domlist_read_lock);
        return false;
    }

    rcu_read_unlock(&domlist_read_lock);

    return true;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
