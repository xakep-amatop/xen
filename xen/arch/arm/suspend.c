// TODOSR add header
#include <xen/sched.h>
#include <xen/cpu.h>
#include <xen/console.h>
#include <xen/iommu.h>
#include <asm/cpufeature.h>
#include <asm/event.h>
#include <asm/psci.h>
#include <asm/suspend.h>
#include <asm/platform.h>
#include <public/sched.h>

struct cpu_context cpu_context;
/* Reset values of VCPU architecture specific registers */
static void vcpu_arch_reset(struct vcpu *v)
{
    v->arch.ttbr0 = 0;
    v->arch.ttbr1 = 0;
    v->arch.ttbcr = 0;

    v->arch.csselr = 0;
    v->arch.cpacr = 0;
    v->arch.contextidr = 0;
    v->arch.tpidr_el0 = 0;
    v->arch.tpidrro_el0 = 0;
    v->arch.tpidr_el1 = 0;
    v->arch.vbar = 0;
    v->arch.dacr = 0;
    v->arch.par = 0;
#if defined(CONFIG_ARM_32)
    v->arch.mair0 = 0;
    v->arch.mair1 = 0;
    v->arch.amair0 = 0;
    v->arch.amair1 = 0;
#else
    v->arch.mair = 0;
    v->arch.amair = 0;
#endif
    /* Fault Status */
#if defined(CONFIG_ARM_32)
    v->arch.dfar = 0;
    v->arch.ifar = 0;
    v->arch.dfsr = 0;
#elif defined(CONFIG_ARM_64)
    v->arch.far = 0;
    v->arch.esr = 0;
#endif

    v->arch.ifsr  = 0;
    v->arch.afsr0 = 0;
    v->arch.afsr1 = 0;

#ifdef CONFIG_ARM_32
    v->arch.joscr = 0;
    v->arch.jmcr = 0;
#endif

    v->arch.teecr = 0;
    v->arch.teehbr = 0;
}

static void vcpu_suspend_prepare(register_t epoint, register_t cid)
{
    struct vcpu *v = current;

    v->arch.suspend_ep = epoint;
    v->arch.suspend_cid = cid;

    return;
}

/*
 * This function sets the context of current VCPU to the state which is expected
 * by the guest on resume. The expected VCPU state is:
 * 1) pc to contain resume entry point (1st argument of PSCI SYSTEM_SUSPEND)
 * 2) r0/x0 to contain context ID (2nd argument of PSCI SYSTEM_SUSPEND)
 * 3) All other general purpose and system registers should have reset values
 */
void vcpu_resume(struct vcpu *v)
{

    struct vcpu_guest_context ctxt;

    /* Make sure that VCPU guest regs are zeroed */
    memset(&ctxt, 0, sizeof(ctxt));

    /* Set non-zero values to the registers prior to copying */
    ctxt.user_regs.pc64 = (u64)v->arch.suspend_ep;

    if ( is_32bit_domain(v->domain) )
    {
        ctxt.user_regs.r0_usr = v->arch.suspend_cid;
        ctxt.user_regs.cpsr = PSR_GUEST32_INIT;

        /* Thumb set is allowed only for 32-bit domain */
        if ( v->arch.suspend_ep & 1 )
        {
            ctxt.user_regs.cpsr |= PSR_THUMB;
            ctxt.user_regs.pc64 &= ~(u64)1;
        }
    }
#ifdef CONFIG_ARM_64
    else
    {
        ctxt.user_regs.x0 = v->arch.suspend_cid;
        ctxt.user_regs.cpsr = PSR_GUEST64_INIT;
    }
#endif
    ctxt.sctlr = SCTLR_GUEST_INIT;
    ctxt.flags = VGCF_online;

    /* Reset architecture specific registers */
    vcpu_arch_reset(v);

    /* Initialize VCPU registers */
    domain_lock(v->domain);
    arch_set_info_guest(v, &ctxt);
    domain_unlock(v->domain);
    watchdog_domain_resume(v->domain);
}

/*
 * After boot, Xen page-tables should not contain mapping that are both
 * Writable and eXecutables.
 *
 * This should be called on each CPU to enforce the policy.
 */
static void xen_pt_enforce_wnx(void)
{
    WRITE_SYSREG(READ_SYSREG(SCTLR_EL2) | SCTLR_Axx_ELx_WXN, SCTLR_EL2);
    /*
     * The TLBs may cache SCTLR_EL2.WXN. So ensure it is synchronized
     * before flushing the TLBs.
     */
    isb();
    flush_xen_tlb_local();
}

#ifndef CONFIG_ARM_64
/* not supported on ARM_32 */
int32_t hyp_suspend(struct cpu_context *ptr) { return 1; }
#endif

#define IMX_SIP_WAKEUP_SRC              0xc2000009
#define IMX_SIP_WAKEUP_SRC_SCU          0x1
#define IMX_SIP_WAKEUP_SRC_IRQSTEER     0x2


/* Xen suspend. Note: data is not used (suspend is the suspend to RAM) */
static long system_suspend(void *data)
{
    int status;
    unsigned long flags;

    BUG_ON(system_state != SYS_STATE_active);

    system_state = SYS_STATE_suspend;
    freeze_domains();

    status = disable_nonboot_cpus();
    if ( status )
    {
        system_state = SYS_STATE_resume;
        goto resume_nonboot_cpus;
    }
    local_irq_save(flags);

    time_suspend();

    status = iommu_suspend();
    if ( status )
    {
        system_state = SYS_STATE_resume;
        goto resume_time;
    }

    status = gic_suspend();
    if ( status )
    {
        //system_state = SYS_STATE_resume;
        //goto resume_irqs;
    }

    dprintk(XENLOG_DEBUG, "Suspend\n");
    // Enable identity mapping before entering suspend to simplify
    // the resume path.
    update_boot_mapping(true);

    status = console_suspend();
    if ( status )
    {
        dprintk(XENLOG_ERR, "Failed to suspend the console, err=%d\n", status);
        system_state = SYS_STATE_resume;
        goto resume_console;
    }

    dprintk(XENLOG_DEBUG, "Suspend 0001\n");

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
            dprintk(XENLOG_ERR, "PSCI system suspend failed, err=%d\n", status);
    }

    dprintk(XENLOG_DEBUG, "Suspend 0002\n");

    system_state = SYS_STATE_resume;

    /*
     * SCTLR_WXN needs to be set to configure that a mapping cannot be both
     * writable and executable.
     */
    xen_pt_enforce_wnx();
    update_boot_mapping(false);

resume_console:
    console_resume();
    dprintk(XENLOG_DEBUG, "Resume\n");

    gic_resume();

//resume_irqs:
    local_irq_restore(flags);

    iommu_resume();

resume_time:
    time_resume();

resume_nonboot_cpus:
    rcu_barrier();
    enable_nonboot_cpus();
    thaw_domains();
    system_state = SYS_STATE_active;
    dsb(sy);

    /* Wake-up hardware domain (should always resume after Xen) */
    vcpu_resume(hardware_domain->vcpu[0]);
    vcpu_unblock(hardware_domain->vcpu[0]);

    return status;
}

int32_t domain_suspend(register_t epoint, register_t cid)
{
    struct vcpu *v;
    struct domain *d = current->domain;
    bool is_thumb = epoint & 1;
    int status;

    dprintk(XENLOG_DEBUG,
            "Dom%d suspend: epoint=0x%"PRIregister", cid=0x%"PRIregister"\n",
            d->domain_id, epoint, cid);

    /* THUMB set is not allowed with 64-bit domain */
    if ( is_64bit_domain(d) && is_thumb )
        return PSCI_INVALID_ADDRESS;

    // TODOSR care about locking here
    /* Ensure that all CPUs other than the calling one are offline */
    for_each_vcpu ( d, v )
    {
        if ( v != current && is_vcpu_online(v) )
            return PSCI_DENIED;
    }

    //TODO: add support for suspending from any VCPU
    if (current->vcpu_id != 0)
        return PSCI_DENIED;

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

    /* If this was dom0 the whole system should suspend: trigger Xen suspend */
    if ( is_hardware_domain(d) )
    {
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
    }

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
