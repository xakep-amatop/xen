/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/psci.h>
#include <asm/suspend.h>
#include <xen/console.h>
#include <xen/cpu.h>
#include <xen/iommu.h>
#include <xen/llc-coloring.h>
#include <xen/sched.h>

/*
 * TODO list:
 *  - Test system suspend with LLC_COLORING enabled and verify functionality
 *  - Implement IOMMU suspend/resume handlers and integrate them
 *    into the suspend/resume path (IPMMU and SMMU)
 *  - Enable "xl suspend" support on ARM architecture
 *  - Properly disable Xen timer watchdog from relevant services
 *  - Add suspend/resume CI test for ARM (QEMU if feasible)
 *  - Investigate feasibility and need for implementing system suspend on ARM32
 */

struct cpu_context cpu_context;

/* Xen suspend. Note: data is not used (suspend is the suspend to RAM) */
static long system_suspend(void *data)
{
    int status;
    unsigned long flags;

    BUG_ON(system_state != SYS_STATE_active);

    system_state = SYS_STATE_suspend;
    freeze_domains();
    scheduler_disable();

    /*
     * Non-boot CPUs have to be disabled on suspend and enabled on resume
     * (hotplug-based mechanism). Disabling non-boot CPUs will lead to PSCI
     * CPU_OFF to be called by each non-boot CPU. Depending on the underlying
     * platform capabilities, this may lead to the physical powering down of
     * CPUs. Tested on Xilinx Zynq Ultrascale+ MPSoC (including power down of
     * each non-boot CPU).
     */
    status = disable_nonboot_cpus();
    if ( status )
    {
        system_state = SYS_STATE_resume;
        goto resume_nonboot_cpus;
    }

    time_suspend();

    status = iommu_suspend();
    if ( status )
    {
        system_state = SYS_STATE_resume;
        goto resume_time;
    }

    local_irq_save(flags);
    status = gic_suspend();
    if ( status )
    {
        system_state = SYS_STATE_resume;
        goto resume_irqs;
    }

    printk("Xen suspending...\n");

    console_start_sync();
    status = console_suspend();
    if ( status )
    {
        dprintk(XENLOG_ERR, "Failed to suspend the console, err=%d\n", status);
        system_state = SYS_STATE_resume;
        goto resume_console;
    }

    set_init_ttbr(xen_pgtable);

    /*
     * Enable identity mapping before entering suspend to simplify
     * the resume path
     */
    update_boot_mapping(true);

    if ( hyp_suspend(&cpu_context) )
    {
        status = call_psci_system_suspend();
        /*
         * If suspend is finalized properly by above system suspend PSCI call,
         * the code below in this 'if' branch will never execute. Execution
         * will continue from hyp_resume which is the hypervisor's resume point.
         * In hyp_resume CPU context will be restored and since link-register is
         * restored as well, it will appear to return from hyp_suspend. The
         * difference in returning from hyp_suspend on system suspend versus
         * resume is in function's return value: on suspend, the return value is
         * a non-zero value, on resume it is zero. That is why the control flow
         * will not re-enter this 'if' branch on resume.
         */
        if ( status )
            dprintk(XENLOG_WARNING, "PSCI system suspend failed, err=%d\n", status);
    }

    system_state = SYS_STATE_resume;
    update_boot_mapping(false);

 resume_console:
    console_resume();
    console_end_sync();

    gic_resume();

 resume_irqs:
    local_irq_restore(flags);

    iommu_resume();

 resume_time:
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

    /* The resume of hardware domain should always follow Xen's resume. */
    domain_resume(hardware_domain);

    printk("Resume (status %d)\n", status);
    return status;
}

int host_system_suspend(void)
{
    int status;

    /* TODO: drop check after verification that features can work together */
    if ( llc_coloring_enabled )
    {
        dprintk(XENLOG_ERR,
                "System suspend is not supported with LLC_COLORING enabled\n");
        return -ENOSYS;
    }

    /* TODO: drop check once suspend/resume support for SMMU is implemented */
#ifndef CONFIG_IPMMU_VMSA
    if ( iommu_enabled )
    {
        dprintk(XENLOG_ERR, "IOMMU is enabled, suspend not supported yet\n");
        return -ENOSYS;
    }
#endif

    /*
     * system_suspend should be called when Dom0 finalizes the suspend
     * procedure from its boot core (VCPU#0). However, Dom0's VCPU#0 could
     * be mapped to any PCPU (this function could be executed by any PCPU).
     * The suspend procedure has to be finalized by the PCPU#0 (non-boot
     * PCPUs will be disabled during the suspend).
     */
    status = continue_hypercall_on_cpu(0, system_suspend, NULL);

    /*
     * If an error happened, there is nothing that needs to be done here
     * because the system_suspend always returns in fully functional state
     * no matter what the outcome of suspend procedure is. If the system
     * suspended successfully the function will return 0 after the resume.
     * Otherwise, if an error is returned it means Xen did not suspended,
     * but it is still in the same state as if the system_suspend was never
     * called. We dump a debug message in case of an error for debugging/
     * logging purpose.
     */
    if ( status )
        dprintk(XENLOG_ERR, "Failed to suspend, errno=%d\n", status);

    return status;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
