/*
 * Copyright (c) 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) Allen Kay <allen.m.kay@intel.com>
 * Copyright (C) Weidong Han <weidong.han@intel.com>
 * Copyright (C) Xiaohui Xin <xiaohui.xin@intel.com>
 */

#include <xen/event.h>
#include <xen/iommu.h>
#include <xen/cpu.h>
#include <xen/irq.h>
#include <asm/hvm/irq.h>
#include <asm/io_apic.h>

/*
 * Gcc12 takes issue with pirq_dpci() being used in boolean context (see gcc
 * bug 102967). While we can't replace the macro definition in the header by an
 * inline function, we can do so here.
 */
static inline struct hvm_pirq_dpci *_pirq_dpci(struct pirq *pirq)
{
    return pirq_dpci(pirq);
}
#undef pirq_dpci
#define pirq_dpci(pirq) _pirq_dpci(pirq)

static DEFINE_PER_CPU(struct list_head, dpci_list);

/*
 * These two bit states help to safely schedule, deschedule, and wait until
 * the softirq has finished.
 *
 * The semantics behind these two bits is as follow:
 *  - STATE_SCHED - whoever modifies it has to ref-count the domain (->dom).
 *  - STATE_RUN - only softirq is allowed to set and clear it. If it has
 *      been set hvm_dirq_assist will RUN with a saved value of the
 *      'struct domain' copied from 'pirq_dpci->dom' before STATE_RUN was set.
 *
 * The usual states are: STATE_SCHED(set) -> STATE_RUN(set) ->
 * STATE_SCHED(unset) -> STATE_RUN(unset).
 *
 * However the states can also diverge such as: STATE_SCHED(set) ->
 * STATE_SCHED(unset) -> STATE_RUN(set) -> STATE_RUN(unset). That means
 * the 'hvm_dirq_assist' never run and that the softirq did not do any
 * ref-counting.
 */

enum {
    STATE_SCHED,
    STATE_RUN
};

/*
 * This can be called multiple times, but the softirq is only raised once.
 * That is until the STATE_SCHED state has been cleared. The state can be
 * cleared by: the 'dpci_softirq' (when it has executed 'hvm_dirq_assist'),
 * or by 'pt_pirq_softirq_reset' (which will try to clear the state before
 * the softirq had a chance to run).
 */
static void raise_softirq_for(struct hvm_pirq_dpci *pirq_dpci)
{
    unsigned long flags;

    if ( test_and_set_bit(STATE_SCHED, &pirq_dpci->state) )
        return;

    get_knownalive_domain(pirq_dpci->dom);

    local_irq_save(flags);
    list_add_tail(&pirq_dpci->softirq_list, &this_cpu(dpci_list));
    local_irq_restore(flags);

    raise_softirq(HVM_DPCI_SOFTIRQ);
}

/*
 * If we are racing with softirq_dpci (STATE_SCHED) we return
 * true. Otherwise we return false.
 *
 * If it is false, it is the callers responsibility to make sure
 * that the softirq (with the event_lock dropped) has ran.
 */
bool pt_pirq_softirq_active(struct hvm_pirq_dpci *pirq_dpci)
{
    if ( pirq_dpci->state & ((1 << STATE_RUN) | (1 << STATE_SCHED)) )
        return true;

    /*
     * If in the future we would call 'raise_softirq_for' right away
     * after 'pt_pirq_softirq_active' we MUST reset the list (otherwise it
     * might have stale data).
     */
    return false;
}

/*
 * Reset the pirq_dpci->dom parameter to NULL.
 *
 * This function checks the different states to make sure it can do it
 * at the right time. If it unschedules the 'hvm_dirq_assist' from running
 * it also refcounts (which is what the softirq would have done) properly.
 */
static void pt_pirq_softirq_reset(struct hvm_pirq_dpci *pirq_dpci)
{
    struct domain *d = pirq_dpci->dom;

    ASSERT(rw_is_write_locked(&d->event_lock));

    switch ( cmpxchg(&pirq_dpci->state, 1 << STATE_SCHED, 0) )
    {
    case (1 << STATE_SCHED):
        /*
         * We are going to try to de-schedule the softirq before it goes in
         * STATE_RUN. Whoever clears STATE_SCHED MUST refcount the 'dom'.
         */
        put_domain(d);
        /* fallthrough. */
    case (1 << STATE_RUN):
    case (1 << STATE_RUN) | (1 << STATE_SCHED):
        /*
         * The reason it is OK to reset 'dom' when STATE_RUN bit is set is due
         * to a shortcut the 'dpci_softirq' implements. It stashes the 'dom'
         * in local variable before it sets STATE_RUN - and therefore will not
         * dereference '->dom' which would crash.
         */
        pirq_dpci->dom = NULL;
        break;
    }
    /*
     * Inhibit 'hvm_dirq_assist' from doing anything useful and at worst
     * calling 'set_timer' which will blow up (as we have called kill_timer
     * or never initialized it). Note that we hold the lock that
     * 'hvm_dirq_assist' could be spinning on.
     */
    pirq_dpci->masked = 0;
}

struct hvm_irq_dpci *domain_get_irq_dpci(const struct domain *d)
{
    if ( !d || !is_hvm_domain(d) )
        return NULL;

    return hvm_domain_irq(d)->dpci;
}

void free_hvm_irq_dpci(struct hvm_irq_dpci *dpci)
{
    xfree(dpci);
}

/*
 * This routine handles lowest-priority interrupts using vector-hashing
 * mechanism. As an example, modern Intel CPUs use this method to handle
 * lowest-priority interrupts.
 *
 * Here is the details about the vector-hashing mechanism:
 * 1. For lowest-priority interrupts, store all the possible destination
 *    vCPUs in an array.
 * 2. Use "gvec % max number of destination vCPUs" to find the right
 *    destination vCPU in the array for the lowest-priority interrupt.
 */
static struct vcpu *vector_hashing_dest(const struct domain *d,
                                        uint32_t dest_id,
                                        bool dest_mode,
                                        uint8_t gvec)

{
    unsigned long *dest_vcpu_bitmap;
    unsigned int dest_vcpus = 0;
    struct vcpu *v, *dest = NULL;
    unsigned int i;

    dest_vcpu_bitmap = xzalloc_array(unsigned long,
                                     BITS_TO_LONGS(d->max_vcpus));
    if ( !dest_vcpu_bitmap )
        return NULL;

    for_each_vcpu ( d, v )
    {
        if ( !vlapic_match_dest(vcpu_vlapic(v), NULL, APIC_DEST_NOSHORT,
                                dest_id, dest_mode) )
            continue;

        __set_bit(v->vcpu_id, dest_vcpu_bitmap);
        dest_vcpus++;
    }

    if ( dest_vcpus != 0 )
    {
        unsigned int mod = gvec % dest_vcpus;
        unsigned int idx = 0;

        for ( i = 0; i <= mod; i++ )
        {
            idx = find_next_bit(dest_vcpu_bitmap, d->max_vcpus, idx) + 1;
            BUG_ON(idx > d->max_vcpus);
        }

        dest = d->vcpu[idx - 1];
    }

    xfree(dest_vcpu_bitmap);

    return dest;
}

int pt_irq_create_bind(
    struct domain *d, const struct xen_domctl_bind_pt_irq *pt_irq_bind)
{
    struct hvm_irq_dpci *hvm_irq_dpci;
    struct hvm_pirq_dpci *pirq_dpci;
    struct pirq *info;
    int rc, pirq = pt_irq_bind->machine_irq;

    if ( pirq < 0 || pirq >= d->nr_pirqs )
        return -EINVAL;

 restart:
    write_lock(&d->event_lock);

    hvm_irq_dpci = domain_get_irq_dpci(d);
    if ( !hvm_irq_dpci && !is_hardware_domain(d) )
    {
        unsigned int i;

        /*
         * NB: the hardware domain doesn't use a hvm_irq_dpci struct because
         * it's only allowed to identity map GSIs, and so the data contained in
         * that struct (used to map guest GSIs into machine GSIs and perform
         * interrupt routing) is completely useless to it.
         */
        hvm_irq_dpci = xzalloc(struct hvm_irq_dpci);
        if ( hvm_irq_dpci == NULL )
        {
            write_unlock(&d->event_lock);
            return -ENOMEM;
        }
        for ( i = 0; i < NR_HVM_DOMU_IRQS; i++ )
            INIT_LIST_HEAD(&hvm_irq_dpci->girq[i]);

        hvm_domain_irq(d)->dpci = hvm_irq_dpci;
    }

    info = pirq_get_info(d, pirq);
    if ( !info )
    {
        write_unlock(&d->event_lock);
        return -ENOMEM;
    }
    pirq_dpci = pirq_dpci(info);

    /*
     * A crude 'while' loop with us dropping the spinlock and giving
     * the softirq_dpci a chance to run.
     * We MUST check for this condition as the softirq could be scheduled
     * and hasn't run yet. Note that this code replaced tasklet_kill which
     * would have spun forever and would do the same thing (wait to flush out
     * outstanding hvm_dirq_assist calls.
     */
    if ( pt_pirq_softirq_active(pirq_dpci) )
    {
        write_unlock(&d->event_lock);
        cpu_relax();
        goto restart;
    }

    switch ( pt_irq_bind->irq_type )
    {
    case PT_IRQ_TYPE_MSI:
    {
        uint8_t dest, delivery_mode;
        bool dest_mode;
        int dest_vcpu_id;
        const struct vcpu *vcpu;
        uint32_t gflags = pt_irq_bind->u.msi.gflags &
                          ~XEN_DOMCTL_VMSI_X86_UNMASKED;

        if ( !(pirq_dpci->flags & HVM_IRQ_DPCI_MAPPED) )
        {
            pirq_dpci->flags = HVM_IRQ_DPCI_MAPPED | HVM_IRQ_DPCI_MACH_MSI |
                               HVM_IRQ_DPCI_GUEST_MSI;
            pirq_dpci->gmsi.gvec = pt_irq_bind->u.msi.gvec;
            pirq_dpci->gmsi.gflags = gflags;
            /*
             * 'pt_irq_create_bind' can be called after 'pt_irq_destroy_bind'.
             * The 'pirq_cleanup_check' which would free the structure is only
             * called if the event channel for the PIRQ is active. However
             * OS-es that use event channels usually bind PIRQs to eventds
             * and unbind them before calling 'pt_irq_destroy_bind' - with the
             * result that we re-use the 'dpci' structure. This can be
             * reproduced with unloading and loading the driver for a device.
             *
             * As such on every 'pt_irq_create_bind' call we MUST set it.
             */
            pirq_dpci->dom = d;
            /* bind after hvm_irq_dpci is setup to avoid race with irq handler*/
            rc = pirq_guest_bind(d->vcpu[0], info, 0);
            if ( rc == 0 && pt_irq_bind->u.msi.gtable )
            {
                rc = msixtbl_pt_register(d, info, pt_irq_bind->u.msi.gtable);
                if ( unlikely(rc) )
                {
                    pirq_guest_unbind(d, info);
                    /*
                     * Between 'pirq_guest_bind' and before 'pirq_guest_unbind'
                     * an interrupt can be scheduled. No more of them are going
                     * to be scheduled but we must deal with the one that may be
                     * in the queue.
                     */
                    pt_pirq_softirq_reset(pirq_dpci);
                }
            }
            if ( unlikely(rc) )
            {
                pirq_dpci->gmsi.gflags = 0;
                pirq_dpci->gmsi.gvec = 0;
                pirq_dpci->dom = NULL;
                pirq_dpci->flags = 0;
                pirq_cleanup_check(info, d);
                write_unlock(&d->event_lock);
                return rc;
            }
        }
        else
        {
            uint32_t mask = HVM_IRQ_DPCI_MACH_MSI | HVM_IRQ_DPCI_GUEST_MSI;

            if ( (pirq_dpci->flags & mask) != mask )
            {
                write_unlock(&d->event_lock);
                return -EBUSY;
            }

            /* If pirq is already mapped as vmsi, update guest data/addr. */
            if ( pirq_dpci->gmsi.gvec != pt_irq_bind->u.msi.gvec ||
                 pirq_dpci->gmsi.gflags != gflags )
            {
                /* Directly clear pending EOIs before enabling new MSI info. */
                pirq_guest_eoi(info);

                pirq_dpci->gmsi.gvec = pt_irq_bind->u.msi.gvec;
                pirq_dpci->gmsi.gflags = gflags;
            }
        }
        /* Calculate dest_vcpu_id for MSI-type pirq migration. */
        dest = MASK_EXTR(pirq_dpci->gmsi.gflags,
                         XEN_DOMCTL_VMSI_X86_DEST_ID_MASK);
        dest_mode = pirq_dpci->gmsi.gflags & XEN_DOMCTL_VMSI_X86_DM_MASK;
        delivery_mode = MASK_EXTR(pirq_dpci->gmsi.gflags,
                                  XEN_DOMCTL_VMSI_X86_DELIV_MASK);

        dest_vcpu_id = hvm_girq_dest_2_vcpu_id(d, dest, dest_mode);
        pirq_dpci->gmsi.dest_vcpu_id = dest_vcpu_id;
        write_unlock(&d->event_lock);

        pirq_dpci->gmsi.posted = false;
        vcpu = (dest_vcpu_id >= 0) ? d->vcpu[dest_vcpu_id] : NULL;
        if ( iommu_intpost )
        {
            if ( delivery_mode == dest_LowestPrio )
                vcpu = vector_hashing_dest(d, dest, dest_mode,
                                           pirq_dpci->gmsi.gvec);
            if ( vcpu )
                pirq_dpci->gmsi.posted = true;
        }
        if ( vcpu && is_iommu_enabled(d) )
            hvm_migrate_pirq(pirq_dpci, vcpu);

        /* Use interrupt posting if it is supported. */
        if ( iommu_intpost )
        {
            rc = hvm_pi_update_irte(vcpu, info, pirq_dpci->gmsi.gvec);

            if ( rc )
            {
                pt_irq_destroy_bind(d, pt_irq_bind);
                return rc;
            }
        }

        if ( pt_irq_bind->u.msi.gflags & XEN_DOMCTL_VMSI_X86_UNMASKED )
        {
            unsigned long flags;
            struct irq_desc *desc = pirq_spin_lock_irq_desc(info, &flags);

            if ( !desc )
            {
                pt_irq_destroy_bind(d, pt_irq_bind);
                return -EINVAL;
            }

            guest_mask_msi_irq(desc, false);
            spin_unlock_irqrestore(&desc->lock, flags);
        }

        break;
    }

    case PT_IRQ_TYPE_PCI:
    case PT_IRQ_TYPE_MSI_TRANSLATE:
    {
        struct dev_intx_gsi_link *digl = NULL;
        struct hvm_girq_dpci_mapping *girq = NULL;
        unsigned int guest_gsi;

        /*
         * Mapping GSIs for the hardware domain is different than doing it for
         * an unpriviledged guest, the hardware domain is only allowed to
         * identity map GSIs, and as such all the data in the u.pci union is
         * discarded.
         */
        if ( hvm_irq_dpci )
        {
            unsigned int link;

            digl = xmalloc(struct dev_intx_gsi_link);
            girq = xmalloc(struct hvm_girq_dpci_mapping);

            if ( !digl || !girq )
            {
                write_unlock(&d->event_lock);
                xfree(girq);
                xfree(digl);
                return -ENOMEM;
            }

            girq->bus = digl->bus = pt_irq_bind->u.pci.bus;
            girq->device = digl->device = pt_irq_bind->u.pci.device;
            girq->intx = digl->intx = pt_irq_bind->u.pci.intx;
            list_add_tail(&digl->list, &pirq_dpci->digl_list);

            guest_gsi = hvm_pci_intx_gsi(digl->device, digl->intx);
            link = hvm_pci_intx_link(digl->device, digl->intx);

            hvm_irq_dpci->link_cnt[link]++;

            girq->machine_gsi = pirq;
            list_add_tail(&girq->list, &hvm_irq_dpci->girq[guest_gsi]);
        }
        else
        {
            ASSERT(is_hardware_domain(d));

            /* MSI_TRANSLATE is not supported for the hardware domain. */
            if ( pt_irq_bind->irq_type != PT_IRQ_TYPE_PCI ||
                 pirq >= hvm_domain_irq(d)->nr_gsis )
            {
                write_unlock(&d->event_lock);

                return -EINVAL;
            }
            guest_gsi = pirq;
        }

        /* Bind the same mirq once in the same domain */
        if ( !(pirq_dpci->flags & HVM_IRQ_DPCI_MAPPED) )
        {
            unsigned int share;

            /* MUST be set, as the pirq_dpci can be re-used. */
            pirq_dpci->dom = d;
            if ( pt_irq_bind->irq_type == PT_IRQ_TYPE_MSI_TRANSLATE )
            {
                pirq_dpci->flags = HVM_IRQ_DPCI_MAPPED |
                                   HVM_IRQ_DPCI_MACH_MSI |
                                   HVM_IRQ_DPCI_GUEST_PCI |
                                   HVM_IRQ_DPCI_TRANSLATE;
                share = 0;
            }
            else    /* PT_IRQ_TYPE_PCI */
            {
                pirq_dpci->flags = HVM_IRQ_DPCI_MAPPED |
                                   HVM_IRQ_DPCI_MACH_PCI |
                                   HVM_IRQ_DPCI_GUEST_PCI;
                if ( !is_hardware_domain(d) )
                    share = BIND_PIRQ__WILL_SHARE;
                else
                {
                    int mask = vioapic_get_mask(d, guest_gsi);
                    int trigger_mode = vioapic_get_trigger_mode(d, guest_gsi);

                    if ( mask < 0 || trigger_mode < 0 )
                    {
                        write_unlock(&d->event_lock);

                        ASSERT_UNREACHABLE();
                        return -EINVAL;
                    }
                    pirq_dpci->flags |= HVM_IRQ_DPCI_IDENTITY_GSI;
                    /*
                     * Check if the corresponding vIO APIC pin is configured
                     * level or edge trigger, level triggered interrupts will
                     * be marked as shareable.
                     */
                    ASSERT(!mask);
                    share = trigger_mode;
                    if ( trigger_mode == VIOAPIC_EDGE_TRIG )
                        /*
                         * Edge IO-APIC interrupt, no EOI or unmask to perform
                         * and hence no timer needed.
                         */
                        pirq_dpci->flags |= HVM_IRQ_DPCI_NO_EOI;
                }
            }

            /* Deal with gsi for legacy devices */
            rc = pirq_guest_bind(d->vcpu[0], info, share);
            if ( unlikely(rc) )
            {
                /*
                 * There is no path for __do_IRQ to schedule softirq as
                 * IRQ_GUEST is not set. As such we can reset 'dom' directly.
                 */
                pirq_dpci->dom = NULL;
                if ( hvm_irq_dpci )
                {
                    unsigned int link;

                    ASSERT(girq && digl);
                    list_del(&girq->list);
                    list_del(&digl->list);
                    link = hvm_pci_intx_link(digl->device, digl->intx);
                    hvm_irq_dpci->link_cnt[link]--;
                }
                pirq_dpci->flags = 0;
                pirq_cleanup_check(info, d);
                write_unlock(&d->event_lock);
                xfree(girq);
                xfree(digl);
                return rc;
            }
        }

        write_unlock(&d->event_lock);

        if ( iommu_verbose )
        {
            char buf[24] = "";

            if ( digl )
                snprintf(buf, ARRAY_SIZE(buf), " dev=%02x.%02x.%u intx=%u",
                         digl->bus, PCI_SLOT(digl->device),
                         PCI_FUNC(digl->device), digl->intx);

            printk(XENLOG_G_INFO "d%d: bind: m_gsi=%u g_gsi=%u%s\n",
                   d->domain_id, pirq, guest_gsi, buf);
        }
        break;
    }

    default:
        write_unlock(&d->event_lock);
        return -EOPNOTSUPP;
    }

    return 0;
}

int pt_irq_destroy_bind(
    struct domain *d, const struct xen_domctl_bind_pt_irq *pt_irq_bind)
{
    struct hvm_irq_dpci *hvm_irq_dpci;
    struct hvm_pirq_dpci *pirq_dpci;
    unsigned int machine_gsi = pt_irq_bind->machine_irq;
    struct pirq *pirq;
    const char *what = NULL;

    switch ( pt_irq_bind->irq_type )
    {
    case PT_IRQ_TYPE_PCI:
    case PT_IRQ_TYPE_MSI_TRANSLATE:
        if ( iommu_verbose )
        {
            unsigned int device = pt_irq_bind->u.pci.device;
            unsigned int intx = pt_irq_bind->u.pci.intx;

            printk(XENLOG_G_INFO
                   "d%d: unbind: m_gsi=%u g_gsi=%u dev=%02x:%02x.%u intx=%u\n",
                   d->domain_id, machine_gsi, hvm_pci_intx_gsi(device, intx),
                   pt_irq_bind->u.pci.bus,
                   PCI_SLOT(device), PCI_FUNC(device), intx);
        }
        break;
    case PT_IRQ_TYPE_MSI:
    {
        unsigned long flags;
        struct irq_desc *desc = domain_spin_lock_irq_desc(d, machine_gsi,
                                                          &flags);

        if ( !desc )
            return -EINVAL;
        /*
         * Leave the MSI masked, so that the state when calling
         * pt_irq_create_bind is consistent across bind/unbinds.
         */
        guest_mask_msi_irq(desc, true);
        spin_unlock_irqrestore(&desc->lock, flags);
        break;
    }

    default:
        return -EOPNOTSUPP;
    }

    write_lock(&d->event_lock);

    hvm_irq_dpci = domain_get_irq_dpci(d);

    if ( !hvm_irq_dpci && !is_hardware_domain(d) )
    {
        write_unlock(&d->event_lock);
        return -EINVAL;
    }

    pirq = pirq_info(d, machine_gsi);
    pirq_dpci = pirq_dpci(pirq);

    if ( hvm_irq_dpci && pt_irq_bind->irq_type != PT_IRQ_TYPE_MSI )
    {
        unsigned int bus = pt_irq_bind->u.pci.bus;
        unsigned int device = pt_irq_bind->u.pci.device;
        unsigned int intx = pt_irq_bind->u.pci.intx;
        unsigned int guest_gsi = hvm_pci_intx_gsi(device, intx);
        unsigned int link = hvm_pci_intx_link(device, intx);
        struct hvm_girq_dpci_mapping *girq;
        struct dev_intx_gsi_link *digl, *tmp;

        list_for_each_entry ( girq, &hvm_irq_dpci->girq[guest_gsi], list )
        {
            if ( girq->bus         == bus &&
                 girq->device      == device &&
                 girq->intx        == intx &&
                 girq->machine_gsi == machine_gsi )
            {
                list_del(&girq->list);
                xfree(girq);
                girq = NULL;
                break;
            }
        }

        if ( girq )
        {
            write_unlock(&d->event_lock);
            return -EINVAL;
        }

        hvm_irq_dpci->link_cnt[link]--;

        /* clear the mirq info */
        if ( pirq_dpci && (pirq_dpci->flags & HVM_IRQ_DPCI_MAPPED) )
        {
            list_for_each_entry_safe ( digl, tmp, &pirq_dpci->digl_list, list )
            {
                if ( digl->bus    == bus &&
                     digl->device == device &&
                     digl->intx   == intx )
                {
                    list_del(&digl->list);
                    xfree(digl);
                }
            }
            what = list_empty(&pirq_dpci->digl_list) ? "final" : "partial";
        }
        else
            what = "bogus";
    }
    else if ( pirq_dpci && pirq_dpci->gmsi.posted )
        hvm_pi_update_irte(NULL, pirq, 0);

    if ( pirq_dpci && (pirq_dpci->flags & HVM_IRQ_DPCI_MAPPED) &&
         list_empty(&pirq_dpci->digl_list) )
    {
        pirq_guest_unbind(d, pirq);
        msixtbl_pt_unregister(d, pirq);
        pirq_dpci->flags = 0;
        /*
         * See comment in pt_irq_create_bind's PT_IRQ_TYPE_MSI before the
         * call to pt_pirq_softirq_reset.
         */
        pt_pirq_softirq_reset(pirq_dpci);

        pirq_cleanup_check(pirq, d);
    }

    write_unlock(&d->event_lock);

    if ( what && iommu_verbose )
    {
        unsigned int device = pt_irq_bind->u.pci.device;
        char buf[24] = "";

        if ( hvm_irq_dpci )
            snprintf(buf, ARRAY_SIZE(buf), " dev=%02x.%02x.%u intx=%u",
                     pt_irq_bind->u.pci.bus, PCI_SLOT(device),
                     PCI_FUNC(device), pt_irq_bind->u.pci.intx);

        printk(XENLOG_G_INFO "d%d %s unmap: m_irq=%u%s\n",
               d->domain_id, what, machine_gsi, buf);
    }

    return 0;
}

void pt_pirq_init(struct domain *d, struct hvm_pirq_dpci *dpci)
{
    INIT_LIST_HEAD(&dpci->digl_list);
    dpci->gmsi.dest_vcpu_id = -1;
}

bool pt_pirq_cleanup_check(struct hvm_pirq_dpci *dpci)
{
    if ( !dpci->flags && !pt_pirq_softirq_active(dpci) )
    {
        dpci->dom = NULL;
        return true;
    }
    return false;
}

int pt_pirq_iterate(struct domain *d,
                    int (*cb)(struct domain *d,
                              struct hvm_pirq_dpci *pirq_dpci, void *arg),
                    void *arg)
{
    int rc = 0;
    unsigned int pirq = 0, n, i;
    struct pirq *pirqs[8];

    ASSERT(rw_is_locked(&d->event_lock));

    do {
        n = radix_tree_gang_lookup(&d->pirq_tree, (void **)pirqs, pirq,
                                   ARRAY_SIZE(pirqs));
        for ( i = 0; i < n; ++i )
        {
            struct hvm_pirq_dpci *pirq_dpci = pirq_dpci(pirqs[i]);

            pirq = pirqs[i]->pirq;
            if ( (pirq_dpci->flags & HVM_IRQ_DPCI_MAPPED) )
            {
                rc = cb(d, pirq_dpci, arg);
                if ( rc )
                    break;
            }
        }
    } while ( !rc && ++pirq < d->nr_pirqs && n == ARRAY_SIZE(pirqs) );

    return rc;
}

int hvm_do_IRQ_dpci(struct domain *d, struct pirq *pirq)
{
    struct hvm_irq_dpci *dpci = domain_get_irq_dpci(d);
    struct hvm_pirq_dpci *pirq_dpci = pirq_dpci(pirq);

    ASSERT(is_hvm_domain(d));

    if ( !is_iommu_enabled(d) || (!is_hardware_domain(d) && !dpci) ||
         !pirq_dpci || !(pirq_dpci->flags & HVM_IRQ_DPCI_MAPPED) )
        return 0;

    pirq_dpci->masked = 1;
    raise_softirq_for(pirq_dpci);
    return 1;
}

/* called with d->event_lock held */
static void __msi_pirq_eoi(struct hvm_pirq_dpci *pirq_dpci)
{
    irq_desc_t *desc;

    if ( (pirq_dpci->flags & HVM_IRQ_DPCI_MAPPED) &&
         (pirq_dpci->flags & HVM_IRQ_DPCI_MACH_MSI) )
    {
        struct pirq *pirq = dpci_pirq(pirq_dpci);

        BUG_ON(!local_irq_is_enabled());
        desc = pirq_spin_lock_irq_desc(pirq, NULL);
        if ( !desc )
            return;
        desc_guest_eoi(desc, pirq);
    }
}

static int cf_check _hvm_dpci_msi_eoi(
    struct domain *d, struct hvm_pirq_dpci *pirq_dpci, void *arg)
{
    int vector = (long)arg;

    if ( (pirq_dpci->flags & HVM_IRQ_DPCI_MACH_MSI) &&
         (pirq_dpci->gmsi.gvec == vector) )
    {
        unsigned int dest = MASK_EXTR(pirq_dpci->gmsi.gflags,
                                      XEN_DOMCTL_VMSI_X86_DEST_ID_MASK);
        bool dest_mode = pirq_dpci->gmsi.gflags & XEN_DOMCTL_VMSI_X86_DM_MASK;

        if ( vlapic_match_dest(vcpu_vlapic(current), NULL, 0, dest,
                               dest_mode) )
        {
            __msi_pirq_eoi(pirq_dpci);
            return 1;
        }
    }

    return 0;
}

void hvm_dpci_msi_eoi(struct domain *d, int vector)
{
    if ( !is_iommu_enabled(d) ||
         (!hvm_domain_irq(d)->dpci && !is_hardware_domain(d)) )
       return;

    read_lock(&d->event_lock);
    pt_pirq_iterate(d, _hvm_dpci_msi_eoi, (void *)(long)vector);
    read_unlock(&d->event_lock);
}

static void hvm_dirq_assist(struct domain *d, struct hvm_pirq_dpci *pirq_dpci)
{
    if ( unlikely(!hvm_domain_irq(d)->dpci) && !is_hardware_domain(d) )
    {
        ASSERT_UNREACHABLE();
        return;
    }

    write_lock(&d->event_lock);
    if ( test_and_clear_bool(pirq_dpci->masked) )
    {
        struct pirq *pirq = dpci_pirq(pirq_dpci);
        const struct dev_intx_gsi_link *digl;

        if ( hvm_domain_use_pirq(d, pirq) )
        {
            send_guest_pirq(d, pirq);

            if ( pirq_dpci->flags & HVM_IRQ_DPCI_GUEST_MSI )
                goto out;
        }

        if ( pirq_dpci->flags & HVM_IRQ_DPCI_GUEST_MSI )
        {
            vmsi_deliver_pirq(d, pirq_dpci);
            goto out;
        }

        list_for_each_entry ( digl, &pirq_dpci->digl_list, list )
        {
            ASSERT(!(pirq_dpci->flags & HVM_IRQ_DPCI_IDENTITY_GSI));
            hvm_pci_intx_assert(d, digl->device, digl->intx);
            pirq_dpci->pending++;
        }

        if ( pirq_dpci->flags & HVM_IRQ_DPCI_IDENTITY_GSI )
        {
            hvm_gsi_assert(d, pirq->pirq);
            if ( pirq_dpci->flags & HVM_IRQ_DPCI_NO_EOI )
                goto out;
            pirq_dpci->pending++;
        }

        if ( pirq_dpci->flags & HVM_IRQ_DPCI_TRANSLATE )
        {
            /* for translated MSI to INTx interrupt, eoi as early as possible */
            __msi_pirq_eoi(pirq_dpci);
            goto out;
        }
    }

 out:
    write_unlock(&d->event_lock);
}

static void hvm_pirq_eoi(struct pirq *pirq)
{
    struct hvm_pirq_dpci *pirq_dpci;

    if ( !pirq )
    {
        ASSERT_UNREACHABLE();
        return;
    }

    pirq_dpci = pirq_dpci(pirq);

    /*
     * No need to get vector lock for timer
     * since interrupt is still not EOIed
     */
    if ( --pirq_dpci->pending ||
         /* When the interrupt source is MSI no Ack should be performed. */
         (pirq_dpci->flags & HVM_IRQ_DPCI_TRANSLATE) )
        return;

    pirq_guest_eoi(pirq);
}

static void __hvm_dpci_eoi(struct domain *d,
                           const struct hvm_girq_dpci_mapping *girq)
{
    struct pirq *pirq = pirq_info(d, girq->machine_gsi);

    if ( !hvm_domain_use_pirq(d, pirq) )
        hvm_pci_intx_deassert(d, girq->device, girq->intx);

    hvm_pirq_eoi(pirq);
}

static void hvm_gsi_eoi(struct domain *d, unsigned int gsi)
{
    struct pirq *pirq = pirq_info(d, gsi);

    /* Check if GSI is actually mapped. */
    if ( !pirq_dpci(pirq) )
        return;

    hvm_gsi_deassert(d, gsi);
    hvm_pirq_eoi(pirq);
}

static int cf_check _hvm_dpci_isairq_eoi(
    struct domain *d, struct hvm_pirq_dpci *pirq_dpci, void *arg)
{
    const struct hvm_irq *hvm_irq = hvm_domain_irq(d);
    unsigned int isairq = (long)arg;
    const struct dev_intx_gsi_link *digl;

    list_for_each_entry ( digl, &pirq_dpci->digl_list, list )
    {
        unsigned int link = hvm_pci_intx_link(digl->device, digl->intx);

        if ( hvm_irq->pci_link.route[link] == isairq )
        {
            hvm_pci_intx_deassert(d, digl->device, digl->intx);
            if ( --pirq_dpci->pending == 0 )
                pirq_guest_eoi(dpci_pirq(pirq_dpci));
        }
    }

    return 0;
}

static void hvm_dpci_isairq_eoi(struct domain *d, unsigned int isairq)
{
    const struct hvm_irq_dpci *dpci = NULL;

    ASSERT(isairq < NR_ISA_IRQS);

    if ( !is_iommu_enabled(d) )
        return;

    write_lock(&d->event_lock);

    dpci = domain_get_irq_dpci(d);

    if ( dpci && test_bit(isairq, dpci->isairq_map) )
    {
        /* Multiple mirq may be mapped to one isa irq */
        pt_pirq_iterate(d, _hvm_dpci_isairq_eoi, (void *)(long)isairq);
    }

    write_unlock(&d->event_lock);
}

void hvm_dpci_eoi(struct domain *d, unsigned int guest_gsi)
{
    const struct hvm_irq_dpci *hvm_irq_dpci;
    const struct hvm_girq_dpci_mapping *girq;

    if ( !is_iommu_enabled(d) )
        return;

    if ( is_hardware_domain(d) )
    {
        write_lock(&d->event_lock);
        hvm_gsi_eoi(d, guest_gsi);
        goto unlock;
    }

    if ( guest_gsi < NR_ISA_IRQS )
    {
        hvm_dpci_isairq_eoi(d, guest_gsi);
        return;
    }

    write_lock(&d->event_lock);
    hvm_irq_dpci = domain_get_irq_dpci(d);

    if ( !hvm_irq_dpci )
        goto unlock;

    list_for_each_entry ( girq, &hvm_irq_dpci->girq[guest_gsi], list )
        __hvm_dpci_eoi(d, girq);

unlock:
    write_unlock(&d->event_lock);
}

static int cf_check pci_clean_dpci_irq(
    struct domain *d, struct hvm_pirq_dpci *pirq_dpci, void *arg)
{
    struct dev_intx_gsi_link *digl, *tmp;

    if ( !pirq_dpci->flags )
        /* Already processed. */
        return 0;

    pirq_guest_unbind(d, dpci_pirq(pirq_dpci));

    list_for_each_entry_safe ( digl, tmp, &pirq_dpci->digl_list, list )
    {
        list_del(&digl->list);
        xfree(digl);
    }
    /* Note the pirq is now unbound. */
    pirq_dpci->flags = 0;

    return pt_pirq_softirq_active(pirq_dpci) ? -ERESTART : 0;
}

int arch_pci_clean_pirqs(struct domain *d)
{
    struct hvm_irq_dpci *hvm_irq_dpci = NULL;

    if ( !is_iommu_enabled(d) )
        return 0;

    if ( !is_hvm_domain(d) )
        return 0;

    write_lock(&d->event_lock);
    hvm_irq_dpci = domain_get_irq_dpci(d);
    if ( hvm_irq_dpci != NULL )
    {
        int ret = pt_pirq_iterate(d, pci_clean_dpci_irq, NULL);

        if ( ret )
        {
            write_unlock(&d->event_lock);
            return ret;
        }

        hvm_domain_irq(d)->dpci = NULL;
        free_hvm_irq_dpci(hvm_irq_dpci);
    }
    write_unlock(&d->event_lock);

    return 0;
}

/*
 * Note: 'pt_pirq_softirq_reset' can clear the STATE_SCHED before we get to
 * doing it. If that is the case we let 'pt_pirq_softirq_reset' do ref-counting.
 */
static void cf_check dpci_softirq(void)
{
    unsigned int cpu = smp_processor_id();
    LIST_HEAD(our_list);

    local_irq_disable();
    list_splice_init(&per_cpu(dpci_list, cpu), &our_list);
    local_irq_enable();

    while ( !list_empty(&our_list) )
    {
        struct hvm_pirq_dpci *pirq_dpci;
        struct domain *d;

        pirq_dpci = list_entry(our_list.next, struct hvm_pirq_dpci, softirq_list);
        list_del(&pirq_dpci->softirq_list);

        d = pirq_dpci->dom;
        smp_mb(); /* 'd' MUST be saved before we set/clear the bits. */
        if ( test_and_set_bit(STATE_RUN, &pirq_dpci->state) )
        {
            unsigned long flags;

            /* Put back on the list and retry. */
            local_irq_save(flags);
            list_add_tail(&pirq_dpci->softirq_list, &this_cpu(dpci_list));
            local_irq_restore(flags);

            raise_softirq(HVM_DPCI_SOFTIRQ);
            continue;
        }
        /*
         * The one who clears STATE_SCHED MUST refcount the domain.
         */
        if ( test_and_clear_bit(STATE_SCHED, &pirq_dpci->state) )
        {
            hvm_dirq_assist(d, pirq_dpci);
            put_domain(d);
        }
        clear_bit(STATE_RUN, &pirq_dpci->state);
    }
}

static int cf_check cpu_callback(
    struct notifier_block *nfb, unsigned long action, void *hcpu)
{
    unsigned int cpu = (unsigned long)hcpu;
    unsigned long flags;

    switch ( action )
    {
    case CPU_UP_PREPARE:
        INIT_LIST_HEAD(&per_cpu(dpci_list, cpu));
        break;

    case CPU_UP_CANCELED:
        ASSERT(list_empty(&per_cpu(dpci_list, cpu)));
        break;

    case CPU_DEAD:
        if ( list_empty(&per_cpu(dpci_list, cpu)) )
            break;
        /* Take whatever dpci interrupts are pending on the dead CPU. */
        local_irq_save(flags);
        list_splice_init(&per_cpu(dpci_list, cpu), &this_cpu(dpci_list));
        local_irq_restore(flags);
        raise_softirq(HVM_DPCI_SOFTIRQ);
        break;
    }

    return NOTIFY_DONE;
}

static struct notifier_block cpu_nfb = {
    .notifier_call = cpu_callback,
};

static int __init cf_check setup_dpci_softirq(void)
{
    unsigned int cpu;

    for_each_online_cpu(cpu)
        INIT_LIST_HEAD(&per_cpu(dpci_list, cpu));

    open_softirq(HVM_DPCI_SOFTIRQ, dpci_softirq);
    register_cpu_notifier(&cpu_nfb);
    return 0;
}
__initcall(setup_dpci_softirq);
