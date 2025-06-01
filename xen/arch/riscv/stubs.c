/* SPDX-License-Identifier: GPL-2.0-only */
#include <xen/cpumask.h>
#include <xen/domain.h>
#include <xen/irq.h>
#include <xen/nodemask.h>
#include <xen/sections.h>
#include <xen/time.h>
#include <public/domctl.h>

#include <asm/current.h>

/* smpboot.c */

cpumask_t cpu_online_map;
cpumask_t cpu_present_map;
cpumask_t cpu_possible_map;

/* ID of the PCPU we're running on */
DEFINE_PER_CPU(unsigned int, cpu_id);
/* XXX these seem awfully x86ish... */
/* representing HT siblings of each logical CPU */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_sibling_mask);
/* representing HT and core siblings of each logical CPU */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_core_mask);

nodemask_t __read_mostly node_online_map = { { [0] = 1UL } };

/* time.c */

int reprogram_timer(s_time_t timeout)
{
    BUG_ON("unimplemented");
}

void send_timer_event(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void domain_set_time_offset(struct domain *d, int64_t time_offset_seconds)
{
    BUG_ON("unimplemented");
}

/* domctl.c */

long arch_do_domctl(struct xen_domctl *domctl, struct domain *d,
                    XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl)
{
    BUG_ON("unimplemented");
}

void arch_get_domain_info(const struct domain *d,
                          struct xen_domctl_getdomaininfo *info)
{
    BUG_ON("unimplemented");
}

void arch_get_info_guest(struct vcpu *v, vcpu_guest_context_u c)
{
    BUG_ON("unimplemented");
}

/* monitor.c */

int arch_monitor_domctl_event(struct domain *d,
                              struct xen_domctl_monitor_op *mop)
{
    BUG_ON("unimplemented");
}

/* smp.c */

void arch_flush_tlb_mask(const cpumask_t *mask)
{
    BUG_ON("unimplemented");
}

void smp_send_event_check_mask(const cpumask_t *mask)
{
    BUG_ON("unimplemented");
}

void smp_send_call_function_mask(const cpumask_t *mask)
{
    BUG_ON("unimplemented");
}

/* irq.c */

struct pirq *alloc_pirq_struct(struct domain *d)
{
    BUG_ON("unimplemented");
}

int pirq_guest_bind(struct vcpu *v, struct pirq *pirq, int will_share)
{
    BUG_ON("unimplemented");
}

void pirq_guest_unbind(struct domain *d, struct pirq *pirq)
{
    BUG_ON("unimplemented");
}

void pirq_set_affinity(struct domain *d, int pirq, const cpumask_t *mask)
{
    BUG_ON("unimplemented");
}

void irq_ack_none(struct irq_desc *desc)
{
    BUG_ON("unimplemented");
}

int arch_init_one_irq_desc(struct irq_desc *desc)
{
    BUG_ON("unimplemented");
}

void smp_send_state_dump(unsigned int cpu)
{
    BUG_ON("unimplemented");
}

/* domain.c */

DEFINE_PER_CPU(struct vcpu *, curr_vcpu);

void context_switch(struct vcpu *prev, struct vcpu *next)
{
    BUG_ON("unimplemented");
}

void continue_running(struct vcpu *same)
{
    BUG_ON("unimplemented");
}

void sync_local_execstate(void)
{
    BUG_ON("unimplemented");
}

void sync_vcpu_execstate(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void startup_cpu_idle_loop(void)
{
    BUG_ON("unimplemented");
}

void free_domain_struct(struct domain *d)
{
    BUG_ON("unimplemented");
}

void dump_pageframe_info(struct domain *d)
{
    BUG_ON("unimplemented");
}

void free_vcpu_struct(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

int arch_vcpu_create(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void arch_vcpu_destroy(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void vcpu_switch_to_aarch64_mode(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

int arch_sanitise_domain_config(struct xen_domctl_createdomain *config)
{
    BUG_ON("unimplemented");
}

int arch_domain_create(struct domain *d,
                       struct xen_domctl_createdomain *config,
                       unsigned int flags)
{
    BUG_ON("unimplemented");
}

int arch_domain_teardown(struct domain *d)
{
    BUG_ON("unimplemented");
}

void arch_domain_destroy(struct domain *d)
{
    BUG_ON("unimplemented");
}

int arch_domain_shutdown(struct domain *d)
{
    BUG_ON("unimplemented");
}

void arch_domain_pause(struct domain *d)
{
    BUG_ON("unimplemented");
}

void arch_domain_unpause(struct domain *d)
{
    BUG_ON("unimplemented");
}

int arch_domain_soft_reset(struct domain *d)
{
    BUG_ON("unimplemented");
}

void arch_domain_creation_finished(struct domain *d)
{
    BUG_ON("unimplemented");
}

int arch_set_info_guest(struct vcpu *v, vcpu_guest_context_u c)
{
    BUG_ON("unimplemented");
}

int arch_initialise_vcpu(struct vcpu *v, XEN_GUEST_HANDLE_PARAM(void) arg)
{
    BUG_ON("unimplemented");
}

int arch_vcpu_reset(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

int domain_relinquish_resources(struct domain *d)
{
    BUG_ON("unimplemented");
}

void arch_dump_domain_info(struct domain *d)
{
    BUG_ON("unimplemented");
}

void arch_dump_vcpu_info(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void vcpu_mark_events_pending(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void vcpu_update_evtchn_irq(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void vcpu_block_unless_event_pending(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

void vcpu_kick(struct vcpu *v)
{
    BUG_ON("unimplemented");
}

struct domain *alloc_domain_struct(void)
{
    BUG_ON("unimplemented");
}

struct vcpu *alloc_vcpu_struct(const struct domain *d)
{
    BUG_ON("unimplemented");
}

unsigned long
hypercall_create_continuation(unsigned int op, const char *format, ...)
{
    BUG_ON("unimplemented");
}

int __init parse_arch_dom0_param(const char *s, const char *e)
{
    BUG_ON("unimplemented");
}

/* guestcopy.c */

unsigned long raw_copy_to_guest(void *to, const void *from, unsigned int len)
{
    BUG_ON("unimplemented");
}

unsigned long raw_copy_from_guest(void *to, const void __user *from,
                                  unsigned int len)
{
    BUG_ON("unimplemented");
}

/* sysctl.c */

long arch_do_sysctl(struct xen_sysctl *sysctl,
                    XEN_GUEST_HANDLE_PARAM(xen_sysctl_t) u_sysctl)
{
    BUG_ON("unimplemented");
}

void arch_do_physinfo(struct xen_sysctl_physinfo *pi)
{
    BUG_ON("unimplemented");
}

/* p2m.c */

int unmap_mmio_regions(struct domain *d,
                       gfn_t start_gfn,
                       unsigned long nr,
                       mfn_t mfn)
{
    BUG_ON("unimplemented");
}

int map_mmio_regions(struct domain *d,
                     gfn_t start_gfn,
                     unsigned long nr,
                     mfn_t mfn)
{
    BUG_ON("unimplemented");
}

int set_foreign_p2m_entry(struct domain *d, const struct domain *fd,
                          unsigned long gfn, mfn_t mfn)
{
    BUG_ON("unimplemented");
}

int guest_physmap_remove_page(struct domain *d, gfn_t gfn, mfn_t mfn,
                              unsigned int page_order)
{
    BUG_ON("unimplemented");
}

/* delay.c */

void udelay(unsigned long usecs)
{
    BUG_ON("unimplemented");
}

/* guest_access.h */

static inline unsigned long raw_clear_guest(void *to, unsigned int len)
{
    BUG_ON("unimplemented");
}

/* smpboot.c */

int __cpu_up(unsigned int cpu)
{
    BUG_ON("unimplemented");
}

void __cpu_disable(void)
{
    BUG_ON("unimplemented");
}

void __cpu_die(unsigned int cpu)
{
    BUG_ON("unimplemented");
}

unsigned long get_upper_mfn_bound(void)
{
    BUG_ON("unimplemented");
}
