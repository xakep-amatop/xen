/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/psci.h>
#include <asm/suspend.h>

#include <xen/console.h>
#include <xen/cpu.h>
#include <xen/llc-coloring.h>
#include <xen/sched.h>
#include <xen/tasklet.h>

/*
 * TODO list:
 *  - Decide which domain will trigger system suspend ctl or hw ?
 *  - Test system suspend with LLC_COLORING enabled and verify functionality
 *  - Implement IOMMU suspend/resume handlers and integrate them
 *    into the suspend/resume path (SMMU)
 *  - Enable "xl suspend" support on ARM architecture
 *  - Properly disable Xen timer watchdog from relevant services (init.d left)
 *  - Add suspend/resume CI test for ARM (QEMU if feasible)
 *  - Investigate feasibility and need for implementing system suspend on ARM32
 */

struct cpu_context cpu_context;

/* Xen suspend. Note: data is not used (suspend is the suspend to RAM) */
static void cf_check system_suspend(void *data)
{
    int status;
    unsigned long flags;
    /* TODO: drop check after verification that features can work together */
    if ( llc_coloring_enabled )
    {
        dprintk(XENLOG_ERR,
                "System suspend is not supported with LLC_COLORING enabled\n");
        status = -ENOSYS;
        goto dom_resume;
    }

    BUG_ON(system_state != SYS_STATE_active);

    system_state = SYS_STATE_suspend;

    printk("Xen suspending...\n");

    freeze_domains();
    scheduler_disable();

    /*
     * Non-boot CPUs have to be disabled on suspend and enabled on resume
     * (hotplug-based mechanism). Disabling non-boot CPUs will lead to PSCI
     * CPU_OFF to be called by each non-boot CPU. Depending on the underlying
     * platform capabilities, this may lead to the physical powering down of
     * CPUs.
     */
    status = disable_nonboot_cpus();
    if ( status )
    {
        system_state = SYS_STATE_resume;
        goto resume_nonboot_cpus;
    }

    time_suspend();

    console_start_sync();
    status = console_suspend();
    if ( status )
    {
        dprintk(XENLOG_ERR, "Failed to suspend the console, err=%d\n", status);
        system_state = SYS_STATE_resume;
        goto resume_console;
    }

    local_irq_save(flags);
    status = gic_suspend();
    if ( status )
    {
        system_state = SYS_STATE_resume;
        goto resume_irqs;
    }

    set_init_ttbr(xen_pgtable);

    /*
     * Enable identity mapping before entering suspend to simplify
     * the resume path
     */
    update_boot_mapping(true);

    if ( prepare_resume_ctx(&cpu_context) )
    {
        status = call_psci_system_suspend();
        /*
         * If suspend is finalized properly by above system suspend PSCI call,
         * the code below in this 'if' branch will never execute. Execution
         * will continue from hyp_resume which is the hypervisor's resume point.
         * In hyp_resume CPU context will be restored and since link-register is
         * restored as well, it will appear to return from prepare_resume_ctx.
         * The difference in returning from prepare_resume_ctx on system suspend
         * versus resume is in function's return value: on suspend, the return
         * value is a non-zero value, on resume it is zero. That is why the
         * control flow will not re-enter this 'if' branch on resume.
         */
        if ( status )
            dprintk(XENLOG_WARNING, "PSCI system suspend failed, err=%d\n",
                    status);
    }

    system_state = SYS_STATE_resume;
    update_boot_mapping(false);

    gic_resume();

 resume_irqs:
    local_irq_restore(flags);

 resume_console:
    console_resume();
    console_end_sync();

    time_resume();

 resume_nonboot_cpus:
    /*
     * The rcu_barrier() has to be added to ensure that the per cpu area is
     * freed before a non-boot CPU tries to initialize it (_free_percpu_area()
     * has to be called before the init_percpu_area()). This scenario occurs
     * when non-boot CPUs are hot-unplugged on suspend and hotplugged on resume.
     */
    rcu_barrier();
    enable_nonboot_cpus();
    scheduler_enable();
    thaw_domains();

    system_state = SYS_STATE_active;

    printk("Resume (status %d)\n", status);

 dom_resume:
    /* The resume of hardware domain should always follow Xen's resume. */
    domain_resume(hardware_domain);
}

static DECLARE_TASKLET(system_suspend_tasklet, system_suspend, NULL);

void host_system_suspend(void)
{
    if ( !is_hardware_domain(d) )
        return;

    /*
     * system_suspend should be called when hardware domain finalizes the
     * suspend procedure from its boot core (VCPU#0). However, Dom0's vCPU#0
     * could be mapped to any pCPU. The suspend procedure has to be finalized
     * by the pCPU#0 (non-boot pCPUs will be disabled during the suspend).
     */
    tasklet_schedule_on_cpu(&system_suspend_tasklet, 0);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
