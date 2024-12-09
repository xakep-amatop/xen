/*
 *  Copyright (C) 2001  MandrakeSoft S.A.
 *
 *    MandrakeSoft S.A.
 *    43, rue d'Aboukir
 *    75002 Paris - France
 *    http://www.linux-mandrake.com/
 *    http://www.mandrakesoft.com/
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; If not, see <http://www.gnu.org/licenses/>.
 *
 * Support for virtual MSI logic
 * Will be merged it with virtual IOAPIC logic, since most is the same
*/

#include <xen/types.h>
#include <xen/mm.h>
#include <xen/xmalloc.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/nospec.h>
#include <xen/sched.h>
#include <xen/softirq.h>
#include <xen/irq.h>
#include <xen/vpci.h>
#include <public/hvm/ioreq.h>
#include <asm/hvm/emulate.h>
#include <asm/hvm/io.h>
#include <asm/hvm/vpic.h>
#include <asm/hvm/vlapic.h>
#include <asm/hvm/support.h>
#include <asm/current.h>
#include <asm/event.h>
#include <asm/io_apic.h>

static void vmsi_inj_irq(
    struct vlapic *target,
    uint8_t vector,
    uint8_t trig_mode,
    uint8_t delivery_mode)
{
    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "vmsi_inj_irq: vec %02x trig %d dm %d\n",
                vector, trig_mode, delivery_mode);

    switch ( delivery_mode )
    {
    case dest_Fixed:
    case dest_LowestPrio:
        vlapic_set_irq(target, vector, trig_mode);
        break;
    default:
        BUG();
    }
}

int vmsi_deliver(
    struct domain *d, int vector,
    uint8_t dest, uint8_t dest_mode,
    uint8_t delivery_mode, uint8_t trig_mode)
{
    struct vlapic *target;
    struct vcpu *v;

    switch ( delivery_mode )
    {
    case dest_LowestPrio:
        target = vlapic_lowest_prio(d, NULL, 0, dest, dest_mode);
        if ( target != NULL )
        {
            vmsi_inj_irq(target, vector, trig_mode, delivery_mode);
            break;
        }
        HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "null MSI round robin: vector=%02x\n",
                    vector);
        return -ESRCH;

    case dest_Fixed:
        for_each_vcpu ( d, v )
        {
            target = vcpu_vlapic(v);
            if ( vlapic_enabled(target) &&
                 vlapic_match_dest(target, NULL, 0, dest, dest_mode) )
                vmsi_inj_irq(target, vector, trig_mode, delivery_mode);
        }
        break;

    default:
        printk(XENLOG_G_WARNING
               "%pv: Unsupported MSI delivery mode %d for Dom%d\n",
               current, delivery_mode, d->domain_id);
        return -EINVAL;
    }

    return 0;
}

void vmsi_deliver_pirq(struct domain *d, const struct hvm_pirq_dpci *pirq_dpci)
{
    uint32_t flags = pirq_dpci->gmsi.gflags;
    int vector = pirq_dpci->gmsi.gvec;
    uint8_t dest = (uint8_t)flags;
    bool dest_mode = flags & XEN_DOMCTL_VMSI_X86_DM_MASK;
    uint8_t delivery_mode = MASK_EXTR(flags, XEN_DOMCTL_VMSI_X86_DELIV_MASK);
    bool trig_mode = flags & XEN_DOMCTL_VMSI_X86_TRIG_MASK;

    HVM_DBG_LOG(DBG_LEVEL_IOAPIC,
                "msi: dest=%x dest_mode=%x delivery_mode=%x "
                "vector=%x trig_mode=%x\n",
                dest, dest_mode, delivery_mode, vector, trig_mode);

    ASSERT(pirq_dpci->flags & HVM_IRQ_DPCI_GUEST_MSI);

    vmsi_deliver(d, vector, dest, dest_mode, delivery_mode, trig_mode);
}

/* Return value, -1 : multi-dests, non-negative value: dest_vcpu_id */
int hvm_girq_dest_2_vcpu_id(struct domain *d, uint8_t dest, uint8_t dest_mode)
{
    int dest_vcpu_id = -1, w = 0;
    struct vcpu *v;
    
    if ( d->max_vcpus == 1 )
        return 0;
 
    for_each_vcpu ( d, v )
    {
        if ( vlapic_match_dest(vcpu_vlapic(v), NULL, 0, dest, dest_mode) ) 
        {
            w++;
            dest_vcpu_id = v->vcpu_id;
        }
    }
    if ( w > 1 )
        return -1;

    return dest_vcpu_id;
}

/* MSI-X mask bit hypervisor interception */
struct msixtbl_entry
{
    struct list_head list;
    atomic_t refcnt;    /* how many bind_pt_irq called for the device */

    /* TODO: resolve the potential race by destruction of pdev */
    struct pci_dev *pdev;
    unsigned long gtable;       /* gpa of msix table */
    DECLARE_BITMAP(table_flags, MAX_MSIX_TABLE_ENTRIES);
#define MAX_MSIX_ACC_ENTRIES 3
    unsigned int table_len;
    struct { 
        uint32_t msi_ad[3];	/* Shadow of address low, high and data */
    } gentries[MAX_MSIX_ACC_ENTRIES];
    DECLARE_BITMAP(acc_valid, 3 * MAX_MSIX_ACC_ENTRIES);
#define acc_bit(what, ent, slot, idx) \
        what##_bit((slot) * 3 + (idx), (ent)->acc_valid)
    struct rcu_head rcu;
};

static DEFINE_RCU_READ_LOCK(msixtbl_rcu_lock);

/*
 * MSI-X table infrastructure is dynamically initialised when an MSI-X capable
 * device is passed through to a domain, rather than unconditionally for all
 * domains.
 */
static bool msixtbl_initialised(const struct domain *d)
{
    return d->arch.hvm.msixtbl_list.next;
}

/*
 * Lookup an msixtbl_entry on the same page as given addr. It's up to the
 * caller to check if address is strictly part of the table - if relevant.
 */
static struct msixtbl_entry *msixtbl_find_entry(
    struct vcpu *v, unsigned long addr)
{
    struct msixtbl_entry *entry;
    struct domain *d = v->domain;

    list_for_each_entry( entry, &d->arch.hvm.msixtbl_list, list )
        if ( PFN_DOWN(addr) >= PFN_DOWN(entry->gtable) &&
             PFN_DOWN(addr) <= PFN_DOWN(entry->gtable + entry->table_len - 1) )
            return entry;

    return NULL;
}

static struct msi_desc *msixtbl_addr_to_desc(
    const struct msixtbl_entry *entry, unsigned long addr)
{
    unsigned int nr_entry;
    struct msi_desc *desc;

    if ( !entry || !entry->pdev )
        return NULL;

    if ( addr <  entry->gtable ||
         addr >= entry->gtable + entry->table_len )
        return NULL;

    nr_entry = (addr - entry->gtable) / PCI_MSIX_ENTRY_SIZE;

    list_for_each_entry( desc, &entry->pdev->msi_list, list )
        if ( desc->msi_attrib.type == PCI_CAP_ID_MSIX &&
             desc->msi_attrib.entry_nr == nr_entry )
            return desc;

    return NULL;
}

/*
 * Returns:
 *  - 0 (FIX_RESERVED) if no handling should be done
 *  - a fixmap idx to use for handling
 */
static unsigned int get_adjacent_idx(
    const struct msixtbl_entry *entry, unsigned long addr, bool write)
{
    unsigned int adj_type;
    struct arch_msix *msix;

    if ( !entry || !entry->pdev )
    {
        ASSERT_UNREACHABLE();
        return 0;
    }

    if ( PFN_DOWN(addr) == PFN_DOWN(entry->gtable) && addr < entry->gtable )
        adj_type = ADJ_IDX_FIRST;
    else if ( PFN_DOWN(addr) == PFN_DOWN(entry->gtable + entry->table_len - 1) &&
              addr >= entry->gtable + entry->table_len )
        adj_type = ADJ_IDX_LAST;
    else
    {
        /* All callers should already do equivalent range checking. */
        ASSERT_UNREACHABLE();
        return 0;
    }

    msix = entry->pdev->msix;
    if ( !msix )
    {
        ASSERT_UNREACHABLE();
        return 0;
    }

    if ( !msix->adj_access_idx[adj_type] )
    {
        if ( MSIX_CHECK_WARN(msix, entry->pdev->domain->domain_id,
                             adjacent_not_initialized) )
            gprintk(XENLOG_WARNING,
                    "%pp: Page for adjacent(%d) MSI-X table access not initialized (addr %#lx, gtable %#lx)\n",
                    &entry->pdev->sbdf, adj_type, addr, entry->gtable);
        return 0;
    }

    /* If PBA lives on the same page too, discard writes. */
    if ( write &&
         ((adj_type == ADJ_IDX_LAST &&
           msix->table.last == msix->pba.first) ||
          (adj_type == ADJ_IDX_FIRST &&
           msix->table.first == msix->pba.last)) )
    {
        if ( MSIX_CHECK_WARN(msix, entry->pdev->domain->domain_id,
                             adjacent_pba) )
            gprintk(XENLOG_WARNING,
                    "%pp: MSI-X table and PBA share a page, "
                    "discard write to adjacent memory (%#lx)\n",
                    &entry->pdev->sbdf, addr);
        return 0;
    }

    return msix->adj_access_idx[adj_type];
}

static void adjacent_read(
    const struct msixtbl_entry *entry,
    paddr_t address, unsigned int len, uint64_t *pval)
{
    const void __iomem *hwaddr;
    unsigned int fixmap_idx;

    ASSERT(IS_ALIGNED(address, len));

    *pval = ~0UL;

    fixmap_idx = get_adjacent_idx(entry, address, false);

    if ( !fixmap_idx )
        return;

    hwaddr = fix_to_virt(fixmap_idx) + PAGE_OFFSET(address);

    switch ( len )
    {
    case 1:
        *pval = readb(hwaddr);
        break;

    case 2:
        *pval = readw(hwaddr);
        break;

    case 4:
        *pval = readl(hwaddr);
        break;

    case 8:
        *pval = readq(hwaddr);
        break;

    default:
        ASSERT_UNREACHABLE();
        break;
    }
}

static void adjacent_write(
    const struct msixtbl_entry *entry,
    paddr_t address, unsigned int len, uint64_t val)
{
    void __iomem *hwaddr;
    unsigned int fixmap_idx;

    ASSERT(IS_ALIGNED(address, len));

    fixmap_idx = get_adjacent_idx(entry, address, true);

    if ( !fixmap_idx )
        return;

    hwaddr = fix_to_virt(fixmap_idx) + PAGE_OFFSET(address);

    switch ( len )
    {
    case 1:
        writeb(val, hwaddr);
        break;

    case 2:
        writew(val, hwaddr);
        break;

    case 4:
        writel(val, hwaddr);
        break;

    case 8:
        writeq(val, hwaddr);
        break;

    default:
        ASSERT_UNREACHABLE();
        break;
    }
}

static int cf_check msixtbl_read(
    const struct hvm_io_handler *handler, uint64_t address, uint32_t len,
    uint64_t *pval)
{
    unsigned long offset;
    struct msixtbl_entry *entry;
    unsigned int nr_entry, index;
    int r = X86EMUL_UNHANDLEABLE;

    if ( !IS_ALIGNED(address, len) )
        return r;

    rcu_read_lock(&msixtbl_rcu_lock);

    entry = msixtbl_find_entry(current, address);
    if ( !entry )
        goto out;

    if ( address <  entry->gtable ||
         address >= entry->gtable + entry->table_len )
    {
        adjacent_read(entry, address, len, pval);
        r = X86EMUL_OKAY;
        goto out;
    }

    if ( len != 4 && len != 8 )
        goto out;

    offset = address & (PCI_MSIX_ENTRY_SIZE - 1);

    if ( offset != PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET )
    {
        nr_entry = (address - entry->gtable) / PCI_MSIX_ENTRY_SIZE;
        index = offset / sizeof(uint32_t);
        if ( nr_entry >= ARRAY_SIZE(entry->gentries) )
            goto out;
        nr_entry = array_index_nospec(nr_entry, ARRAY_SIZE(entry->gentries));
        if ( !acc_bit(test, entry, nr_entry, index) )
            goto out;
        *pval = entry->gentries[nr_entry].msi_ad[index];
        if ( len == 8 )
        {
            if ( index )
                offset = PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET;
            else if ( acc_bit(test, entry, nr_entry, 1) )
                *pval |= (u64)entry->gentries[nr_entry].msi_ad[1] << 32;
            else
                goto out;
        }
    }
    if ( offset == PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET )
    {
        const struct msi_desc *msi_desc = msixtbl_addr_to_desc(entry, address);

        if ( !msi_desc )
            goto out;
        if ( len == 4 )
            *pval = MASK_INSR(msi_desc->msi_attrib.guest_masked,
                              PCI_MSIX_VECTOR_BITMASK);
        else
            *pval |= (u64)MASK_INSR(msi_desc->msi_attrib.guest_masked,
                                    PCI_MSIX_VECTOR_BITMASK) << 32;
    }
    
    r = X86EMUL_OKAY;
out:
    rcu_read_unlock(&msixtbl_rcu_lock);
    return r;
}

static int msixtbl_write(struct vcpu *v, unsigned long address,
                         unsigned int len, unsigned long val)
{
    unsigned long offset;
    struct msixtbl_entry *entry;
    const struct msi_desc *msi_desc;
    unsigned int nr_entry, index;
    int r = X86EMUL_UNHANDLEABLE;
    unsigned long flags;
    struct irq_desc *desc;

    if ( !IS_ALIGNED(address, len) )
        return X86EMUL_OKAY;

    rcu_read_lock(&msixtbl_rcu_lock);

    entry = msixtbl_find_entry(v, address);
    if ( !entry )
        goto out;

    if ( address <  entry->gtable ||
         address >= entry->gtable + entry->table_len )
    {
        adjacent_write(entry, address, len, val);
        r = X86EMUL_OKAY;
        goto out;
    }

    if ( len != 4 && len != 8 )
        goto out;

    nr_entry = array_index_nospec(((address - entry->gtable) /
                                   PCI_MSIX_ENTRY_SIZE),
                                  MAX_MSIX_TABLE_ENTRIES);

    offset = address & (PCI_MSIX_ENTRY_SIZE - 1);
    if ( offset != PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET )
    {
        index = offset / sizeof(uint32_t);
        if ( nr_entry < ARRAY_SIZE(entry->gentries) )
        {
            nr_entry = array_index_nospec(nr_entry,
                                          ARRAY_SIZE(entry->gentries));
            entry->gentries[nr_entry].msi_ad[index] = val;
            acc_bit(set, entry, nr_entry, index);
            if ( len == 8 && !index )
            {
                entry->gentries[nr_entry].msi_ad[1] = val >> 32;
                acc_bit(set, entry, nr_entry, 1);
            }
        }
        set_bit(nr_entry, &entry->table_flags);
        if ( len != 8 || !index )
            goto out;
        val >>= 32;
        address += 4;
    }

    /* Exit to device model when unmasking and address/data got modified. */
    if ( !(val & PCI_MSIX_VECTOR_BITMASK) &&
         test_and_clear_bit(nr_entry, &entry->table_flags) )
    {
        v->arch.hvm.hvm_io.msix_unmask_address = address;
        goto out;
    }

    msi_desc = msixtbl_addr_to_desc(entry, address);
    if ( !msi_desc || msi_desc->irq < 0 )
        goto out;
    
    desc = irq_to_desc(msi_desc->irq);
    if ( !desc )
        goto out;

    spin_lock_irqsave(&desc->lock, flags);

    if ( !desc->msi_desc )
        goto unlock;

    ASSERT(msi_desc == desc->msi_desc);
   
    guest_mask_msi_irq(desc, !!(val & PCI_MSIX_VECTOR_BITMASK));

unlock:
    spin_unlock_irqrestore(&desc->lock, flags);
    r = X86EMUL_OKAY;

out:
    rcu_read_unlock(&msixtbl_rcu_lock);
    return r;
}

static int cf_check _msixtbl_write(
    const struct hvm_io_handler *handler, uint64_t address, uint32_t len,
    uint64_t val)
{
    /* Ignore unaligned writes. */
    if ( !IS_ALIGNED(address, len) )
        return X86EMUL_OKAY;

    /*
     * This function returns X86EMUL_UNHANDLEABLE even if write is properly
     * handled, to propagate it to the device model (so it can keep its
     * internal state in sync).
     */
    msixtbl_write(current, address, len, val);
    return X86EMUL_UNHANDLEABLE;
}

static bool cf_check msixtbl_range(
    const struct hvm_io_handler *handler, const ioreq_t *r)
{
    struct vcpu *curr = current;
    unsigned long addr = r->addr;
    const struct msixtbl_entry *entry;
    bool ret = false;

    ASSERT(r->type == IOREQ_TYPE_COPY);

    rcu_read_lock(&msixtbl_rcu_lock);
    entry = msixtbl_find_entry(curr, addr);
    if ( entry &&
          /* Adjacent access. */
         (addr < entry->gtable || addr >= entry->gtable + entry->table_len ||
          /* Otherwise check if there is a matching msi_desc. */
          msixtbl_addr_to_desc(entry, addr)) )
        ret = true;
    rcu_read_unlock(&msixtbl_rcu_lock);

    if ( ret )
        return ret;

    if ( r->state == STATE_IOREQ_READY && r->dir == IOREQ_WRITE )
    {
        unsigned int size = r->size;

        if ( !r->data_is_ptr )
        {
            uint64_t data = r->data;

            if ( size == 8 )
            {
                BUILD_BUG_ON(!(PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET & 4));
                data >>= 32;
                addr += size = 4;
            }
            if ( size == 4 &&
                 ((addr & (PCI_MSIX_ENTRY_SIZE - 1)) ==
                  PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET) &&
                 !(data & PCI_MSIX_VECTOR_BITMASK) )
            {
                curr->arch.hvm.hvm_io.msix_snoop_address = addr;
                curr->arch.hvm.hvm_io.msix_snoop_gpa = 0;
            }
        }
        else if ( (size == 4 || size == 8) &&
                  /* Only support forward REP MOVS for now. */
                  !r->df &&
                  /*
                   * Only fully support accesses to a single table entry for
                   * now (if multiple ones get written to in one go, only the
                   * final one gets dealt with).
                   */
                  r->count && r->count <= PCI_MSIX_ENTRY_SIZE / size &&
                  !((addr + (size * r->count)) & (PCI_MSIX_ENTRY_SIZE - 1)) )
        {
            BUILD_BUG_ON((PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET + 4) &
                         (PCI_MSIX_ENTRY_SIZE - 1));

            curr->arch.hvm.hvm_io.msix_snoop_address =
                addr + size * r->count - 4;
            curr->arch.hvm.hvm_io.msix_snoop_gpa =
                r->data + size * r->count - 4;
        }
    }

    return false;
}

static const struct hvm_io_ops msixtbl_mmio_ops = {
    .accept = msixtbl_range,
    .read = msixtbl_read,
    .write = _msixtbl_write,
};

static void add_msixtbl_entry(struct domain *d,
                              struct pci_dev *pdev,
                              uint64_t gtable,
                              struct msixtbl_entry *entry)
{
    INIT_LIST_HEAD(&entry->list);
    INIT_RCU_HEAD(&entry->rcu);
    atomic_set(&entry->refcnt, 0);

    entry->table_len = pdev->msix->nr_entries * PCI_MSIX_ENTRY_SIZE;
    entry->pdev = pdev;
    entry->gtable = (unsigned long) gtable;

    list_add_rcu(&entry->list, &d->arch.hvm.msixtbl_list);
}

static void cf_check free_msixtbl_entry(struct rcu_head *rcu)
{
    struct msixtbl_entry *entry;

    entry = container_of (rcu, struct msixtbl_entry, rcu);

    xfree(entry);
}

static void del_msixtbl_entry(struct msixtbl_entry *entry)
{
    list_del_rcu(&entry->list);
    call_rcu(&entry->rcu, free_msixtbl_entry);
}

int msixtbl_pt_register(struct domain *d, struct pirq *pirq, uint64_t gtable)
{
    struct irq_desc *irqd;
    struct msi_desc *msi_desc;
    struct pci_dev *pdev;
    struct msixtbl_entry *entry, *new_entry;
    int r = -EINVAL;

    ASSERT_PDEV_LIST_IS_READ_LOCKED(d);
    ASSERT(rw_is_write_locked(&d->event_lock));

    if ( !msixtbl_initialised(d) )
        return -ENODEV;

    /*
     * xmalloc() with irq_disabled causes the failure of check_lock() 
     * for xenpool->lock. So we allocate an entry beforehand.
     */
    new_entry = xzalloc(struct msixtbl_entry);
    if ( !new_entry )
        return -ENOMEM;

    irqd = pirq_spin_lock_irq_desc(pirq, NULL);
    if ( !irqd )
    {
        xfree(new_entry);
        return r;
    }

    msi_desc = irqd->msi_desc;
    if ( !msi_desc )
        goto out;

    pdev = msi_desc->dev;

    list_for_each_entry( entry, &d->arch.hvm.msixtbl_list, list )
        if ( pdev == entry->pdev )
            goto found;

    entry = new_entry;
    new_entry = NULL;
    add_msixtbl_entry(d, pdev, gtable, entry);

found:
    atomic_inc(&entry->refcnt);
    r = 0;

out:
    spin_unlock_irq(&irqd->lock);
    xfree(new_entry);

    if ( !r )
    {
        struct vcpu *v;

        for_each_vcpu ( d, v )
        {
            if ( (v->pause_flags & VPF_blocked_in_xen) &&
                 !v->arch.hvm.hvm_io.msix_snoop_gpa &&
                 v->arch.hvm.hvm_io.msix_snoop_address ==
                 (gtable + msi_desc->msi_attrib.entry_nr *
                           PCI_MSIX_ENTRY_SIZE +
                  PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET) )
                v->arch.hvm.hvm_io.msix_unmask_address =
                    v->arch.hvm.hvm_io.msix_snoop_address;
        }
    }

    return r;
}

void msixtbl_pt_unregister(struct domain *d, struct pirq *pirq)
{
    struct irq_desc *irqd;
    struct msi_desc *msi_desc;
    struct pci_dev *pdev;
    struct msixtbl_entry *entry;

    ASSERT_PDEV_LIST_IS_READ_LOCKED(d);
    ASSERT(rw_is_write_locked(&d->event_lock));

    if ( !msixtbl_initialised(d) )
        return;

    irqd = pirq_spin_lock_irq_desc(pirq, NULL);
    if ( !irqd )
        return;

    msi_desc = irqd->msi_desc;
    if ( !msi_desc )
        goto out;

    pdev = msi_desc->dev;

    list_for_each_entry( entry, &d->arch.hvm.msixtbl_list, list )
        if ( pdev == entry->pdev )
            goto found;

out:
    spin_unlock_irq(&irqd->lock);
    return;

found:
    if ( !atomic_dec_and_test(&entry->refcnt) )
        del_msixtbl_entry(entry);

    spin_unlock_irq(&irqd->lock);
}

void msixtbl_init(struct domain *d)
{
    struct hvm_io_handler *handler;

    if ( !is_hvm_domain(d) || !has_vlapic(d) || msixtbl_initialised(d) )
        return;

    INIT_LIST_HEAD(&d->arch.hvm.msixtbl_list);

    handler = hvm_next_io_handler(d);
    if ( handler )
    {
        handler->type = IOREQ_TYPE_COPY;
        handler->ops = &msixtbl_mmio_ops;
    }
}

void msixtbl_pt_cleanup(struct domain *d)
{
    struct msixtbl_entry *entry, *temp;

    if ( !msixtbl_initialised(d) )
        return;

    write_lock(&d->event_lock);

    list_for_each_entry_safe( entry, temp,
                              &d->arch.hvm.msixtbl_list, list )
        del_msixtbl_entry(entry);

    write_unlock(&d->event_lock);
}

void msix_write_completion(struct vcpu *v)
{
    unsigned long ctrl_address = v->arch.hvm.hvm_io.msix_unmask_address;
    unsigned long snoop_addr = v->arch.hvm.hvm_io.msix_snoop_address;

    v->arch.hvm.hvm_io.msix_snoop_address = 0;

    if ( !ctrl_address && snoop_addr &&
         v->arch.hvm.hvm_io.msix_snoop_gpa )
    {
        unsigned int token = hvmemul_cache_disable(v);
        const struct msi_desc *desc;
        uint32_t data;

        rcu_read_lock(&msixtbl_rcu_lock);
        desc = msixtbl_addr_to_desc(msixtbl_find_entry(v, snoop_addr),
                                    snoop_addr);
        rcu_read_unlock(&msixtbl_rcu_lock);

        if ( desc &&
             hvm_copy_from_guest_phys(&data,
                                      v->arch.hvm.hvm_io.msix_snoop_gpa,
                                      sizeof(data)) == HVMTRANS_okay &&
             !(data & PCI_MSIX_VECTOR_BITMASK) )
            ctrl_address = snoop_addr;

        hvmemul_cache_restore(v, token);
    }

    if ( !ctrl_address )
        return;

    v->arch.hvm.hvm_io.msix_unmask_address = 0;
    if ( msixtbl_write(v, ctrl_address, 4, 0) != X86EMUL_OKAY )
        gdprintk(XENLOG_WARNING, "MSI-X write completion failure\n");
}

#ifdef CONFIG_HAS_VPCI
static unsigned int msi_gflags(uint16_t data, uint64_t addr, bool masked)
{
    /*
     * We need to use the DOMCTL constants here because the output of this
     * function is used as input to pt_irq_create_bind, which also takes the
     * input from the DOMCTL itself.
     */
    return MASK_INSR(MASK_EXTR(addr, MSI_ADDR_DEST_ID_MASK),
                     XEN_DOMCTL_VMSI_X86_DEST_ID_MASK) |
           MASK_INSR(MASK_EXTR(addr, MSI_ADDR_REDIRECTION_MASK),
                     XEN_DOMCTL_VMSI_X86_RH_MASK) |
           MASK_INSR(MASK_EXTR(addr, MSI_ADDR_DESTMODE_MASK),
                     XEN_DOMCTL_VMSI_X86_DM_MASK) |
           MASK_INSR(MASK_EXTR(data, MSI_DATA_DELIVERY_MODE_MASK),
                     XEN_DOMCTL_VMSI_X86_DELIV_MASK) |
           MASK_INSR(MASK_EXTR(data, MSI_DATA_TRIGGER_MASK),
                     XEN_DOMCTL_VMSI_X86_TRIG_MASK) |
           /* NB: by default MSI vectors are bound masked. */
           (masked ? 0 : XEN_DOMCTL_VMSI_X86_UNMASKED);
}

static void vpci_mask_pirq(struct domain *d, int pirq, bool mask)
{
    unsigned long flags;
    struct irq_desc *desc = domain_spin_lock_irq_desc(d, pirq, &flags);

    if ( !desc )
        return;
    guest_mask_msi_irq(desc, mask);
    spin_unlock_irqrestore(&desc->lock, flags);
}

void vpci_msi_arch_mask(struct vpci_msi *msi, const struct pci_dev *pdev,
                        unsigned int entry, bool mask)
{
    vpci_mask_pirq(pdev->domain, msi->arch.pirq + entry, mask);
}

static int vpci_msi_update(const struct pci_dev *pdev, uint32_t data,
                           uint64_t address, unsigned int vectors,
                           unsigned int pirq, uint32_t mask)
{
    unsigned int i;

    ASSERT_PDEV_LIST_IS_READ_LOCKED(pdev->domain);

    if ( (address & MSI_ADDR_BASE_MASK) != MSI_ADDR_HEADER )
    {
        gdprintk(XENLOG_ERR, "%pp: PIRQ %u: unsupported address %lx\n",
                 &pdev->sbdf, pirq, address);
        return -EOPNOTSUPP;
    }

    for ( i = 0; i < vectors; i++ )
    {
        uint8_t vector = MASK_EXTR(data, MSI_DATA_VECTOR_MASK);
        uint8_t vector_mask = 0xff >> (8 - fls(vectors) + 1);
        struct xen_domctl_bind_pt_irq bind = {
            .machine_irq = pirq + i,
            .irq_type = PT_IRQ_TYPE_MSI,
            .u.msi.gvec = (vector & ~vector_mask) |
                          ((vector + i) & vector_mask),
            .u.msi.gflags = msi_gflags(data, address, (mask >> i) & 1),
        };
        int rc = pt_irq_create_bind(pdev->domain, &bind);

        if ( rc )
        {
            gdprintk(XENLOG_ERR, "%pp: failed to bind PIRQ %u: %d\n",
                     &pdev->sbdf, pirq + i, rc);
            while ( bind.machine_irq-- > pirq )
                pt_irq_destroy_bind(pdev->domain, &bind);
            return rc;
        }
    }

    return 0;
}

void vpci_msi_arch_update(struct vpci_msi *msi, const struct pci_dev *pdev)
{
    unsigned int i;
    int rc;

    ASSERT(msi->arch.pirq != INVALID_PIRQ);
    ASSERT_PDEV_LIST_IS_READ_LOCKED(pdev->domain);

    for ( i = 0; i < msi->vectors && msi->arch.bound; i++ )
    {
        struct xen_domctl_bind_pt_irq unbind = {
            .machine_irq = msi->arch.pirq + i,
            .irq_type = PT_IRQ_TYPE_MSI,
        };

        rc = pt_irq_destroy_bind(pdev->domain, &unbind);
        if ( rc )
        {
            ASSERT_UNREACHABLE();
            domain_crash(pdev->domain);
            return;
        }
    }

    msi->arch.bound = !vpci_msi_update(pdev, msi->data, msi->address,
                                       msi->vectors, msi->arch.pirq, msi->mask);
}

static int vpci_msi_enable(const struct pci_dev *pdev, unsigned int nr,
                           paddr_t table_base)
{
    struct msi_info msi_info = {
        .sbdf = pdev->sbdf,
        .table_base = table_base,
        .entry_nr = nr,
    };
    int rc, pirq = INVALID_PIRQ;

    /* Get a PIRQ. */
    rc = allocate_and_map_msi_pirq(pdev->domain, -1, &pirq,
                                   table_base ? MAP_PIRQ_TYPE_MSI
                                              : MAP_PIRQ_TYPE_MULTI_MSI,
                                   &msi_info);
    if ( rc )
    {
        gdprintk(XENLOG_ERR, "%pp: failed to map PIRQ: %d\n", &pdev->sbdf, rc);
        return rc;
    }

    return pirq;
}

int vpci_msi_arch_enable(struct vpci_msi *msi, const struct pci_dev *pdev,
                         unsigned int vectors)
{
    int rc;

    ASSERT(msi->arch.pirq == INVALID_PIRQ);
    ASSERT_PDEV_LIST_IS_READ_LOCKED(pdev->domain);
    rc = vpci_msi_enable(pdev, vectors, 0);
    if ( rc < 0 )
        return rc;
    msi->arch.pirq = rc;

    msi->arch.bound = !vpci_msi_update(pdev, msi->data, msi->address, vectors,
                                       msi->arch.pirq, msi->mask);

    return 0;
}

static void vpci_msi_disable(const struct pci_dev *pdev, int pirq,
                             unsigned int nr, bool bound)
{
    unsigned int i;

    ASSERT(pirq != INVALID_PIRQ);
    ASSERT_PDEV_LIST_IS_READ_LOCKED(pdev->domain);

    for ( i = 0; i < nr && bound; i++ )
    {
        struct xen_domctl_bind_pt_irq bind = {
            .machine_irq = pirq + i,
            .irq_type = PT_IRQ_TYPE_MSI,
        };
        int rc;

        rc = pt_irq_destroy_bind(pdev->domain, &bind);
        ASSERT(!rc);
    }

    write_lock(&pdev->domain->event_lock);
    unmap_domain_pirq(pdev->domain, pirq);
    write_unlock(&pdev->domain->event_lock);
}

void vpci_msi_arch_disable(struct vpci_msi *msi, const struct pci_dev *pdev)
{
    vpci_msi_disable(pdev, msi->arch.pirq, msi->vectors, msi->arch.bound);
    msi->arch.pirq = INVALID_PIRQ;
}

void vpci_msi_arch_init(struct vpci_msi *msi)
{
    msi->arch.pirq = INVALID_PIRQ;
}

void vpci_msi_arch_print(const struct vpci_msi *msi)
{
    printk("vec=%#02x%7s%6s%3sassert%5s%7s dest_id=%lu pirq: %d\n",
           MASK_EXTR(msi->data, MSI_DATA_VECTOR_MASK),
           msi->data & MSI_DATA_DELIVERY_LOWPRI ? "lowest" : "fixed",
           msi->data & MSI_DATA_TRIGGER_LEVEL ? "level" : "edge",
           msi->data & MSI_DATA_LEVEL_ASSERT ? "" : "de",
           msi->address & MSI_ADDR_DESTMODE_LOGIC ? "log" : "phys",
           msi->address & MSI_ADDR_REDIRECTION_LOWPRI ? "lowest" : "fixed",
           MASK_EXTR(msi->address, MSI_ADDR_DEST_ID_MASK),
           msi->arch.pirq);
}

void vpci_msix_arch_mask_entry(struct vpci_msix_entry *entry,
                               const struct pci_dev *pdev, bool mask)
{
    if ( entry->arch.pirq != INVALID_PIRQ )
        vpci_mask_pirq(pdev->domain, entry->arch.pirq, mask);
}

int vpci_msix_arch_enable_entry(struct vpci_msix_entry *entry,
                                const struct pci_dev *pdev, paddr_t table_base)
{
    int rc;

    ASSERT(entry->arch.pirq == INVALID_PIRQ);
    ASSERT_PDEV_LIST_IS_READ_LOCKED(pdev->domain);
    rc = vpci_msi_enable(pdev, vmsix_entry_nr(pdev->vpci->msix, entry),
                         table_base);
    if ( rc < 0 )
        return rc;

    entry->arch.pirq = rc;

    rc = vpci_msi_update(pdev, entry->data, entry->addr, 1, entry->arch.pirq,
                         entry->masked);
    if ( rc )
    {
        vpci_msi_disable(pdev, entry->arch.pirq, 1, false);
        entry->arch.pirq = INVALID_PIRQ;
    }

    return rc;
}

int vpci_msix_arch_disable_entry(struct vpci_msix_entry *entry,
                                 const struct pci_dev *pdev)
{
    if ( entry->arch.pirq == INVALID_PIRQ )
        return -ENOENT;

    vpci_msi_disable(pdev, entry->arch.pirq, 1, true);
    entry->arch.pirq = INVALID_PIRQ;

    return 0;
}

void vpci_msix_arch_init_entry(struct vpci_msix_entry *entry)
{
    entry->arch.pirq = INVALID_PIRQ;
}

int vpci_msix_arch_print(const struct vpci_msix *msix)
{
    unsigned int i;

    /*
     * Assert that pdev_list doesn't change. ASSERT_PDEV_LIST_IS_READ_LOCKED
     * is not suitable here because it allows either pcidevs_lock() or
     * pci_lock to be held, but here we rely on pci_lock being held, not
     * pcidevs_lock() (see the transient lock dropping further down).
     */
    ASSERT(rw_is_locked(&msix->pdev->domain->pci_lock));
    ASSERT(spin_is_locked(&msix->pdev->vpci->lock));

    for ( i = 0; i < msix->max_entries; i++ )
    {
        const struct vpci_msix_entry *entry = &msix->entries[i];

        printk("%6u vec=%02x%7s%6s%3sassert%5s%7s dest_id=%lu mask=%u pirq: %d\n",
               i, MASK_EXTR(entry->data, MSI_DATA_VECTOR_MASK),
               entry->data & MSI_DATA_DELIVERY_LOWPRI ? "lowest" : "fixed",
               entry->data & MSI_DATA_TRIGGER_LEVEL ? "level" : "edge",
               entry->data & MSI_DATA_LEVEL_ASSERT ? "" : "de",
               entry->addr & MSI_ADDR_DESTMODE_LOGIC ? "log" : "phys",
               entry->addr & MSI_ADDR_REDIRECTION_LOWPRI ? "lowest" : "fixed",
               MASK_EXTR(entry->addr, MSI_ADDR_DEST_ID_MASK),
               entry->masked, entry->arch.pirq);
        if ( i && !(i % 64) )
        {
            struct pci_dev *pdev = msix->pdev;

            spin_unlock(&msix->pdev->vpci->lock);
            read_unlock(&pdev->domain->pci_lock);
            process_pending_softirqs();

            if ( !read_trylock(&pdev->domain->pci_lock) )
                return -EBUSY;

            /* NB: we assume that pdev cannot go away for an alive domain. */
            if ( !pdev->vpci || !spin_trylock(&pdev->vpci->lock) )
            {
                read_unlock(&pdev->domain->pci_lock);
                return -EBUSY;
            }

            if ( pdev->vpci->msix != msix )
            {
                spin_unlock(&pdev->vpci->lock);
                read_unlock(&pdev->domain->pci_lock);
                return -EAGAIN;
            }
        }
    }

    return 0;
}

int vpci_make_msix_hole(const struct pci_dev *pdev)
{
    struct domain *d = pdev->domain;
    unsigned int i;

    if ( !pdev->vpci->msix )
        return 0;

    /* Make sure there's a hole for the MSIX table/PBA in the p2m. */
    for ( i = 0; i < ARRAY_SIZE(pdev->vpci->msix->tables); i++ )
    {
        unsigned long start = PFN_DOWN(vmsix_table_addr(pdev->vpci, i));
        unsigned long end = PFN_DOWN(vmsix_table_addr(pdev->vpci, i) +
                                     vmsix_table_size(pdev->vpci, i) - 1);

        for ( ; start <= end; start++ )
        {
            p2m_type_t t;
            mfn_t mfn = get_gfn_query(d, start, &t);

            switch ( t )
            {
            case p2m_mmio_dm:
            case p2m_invalid:
                break;
            case p2m_mmio_direct:
                if ( mfn_x(mfn) == start )
                {
                    p2m_remove_identity_entry(d, start);
                    break;
                }
                /* fallthrough. */
            default:
                put_gfn(d, start);
                gprintk(XENLOG_WARNING,
                        "%pp: existing mapping (mfn: %" PRI_mfn
                        "type: %d) at %#lx clobbers MSIX MMIO area\n",
                        &pdev->sbdf, mfn_x(mfn), t, start);
                return -EEXIST;
            }
            put_gfn(d, start);
        }
    }

    if ( is_hardware_domain(d) )
    {
        /*
         * For dom0 only: remove any hypervisor mappings of the MSIX or PBA
         * related areas, as dom0 is capable of moving the position of the BARs
         * in the host address space.
         *
         * We rely on being called with the vPCI lock held once the domain is
         * running, so the maps are not in use.
         */
        for ( i = 0; i < ARRAY_SIZE(pdev->vpci->msix->table); i++ )
            if ( pdev->vpci->msix->table[i] )
            {
                /* If there are any maps, the domain must be running. */
                ASSERT(spin_is_locked(&pdev->vpci->lock));
                iounmap(pdev->vpci->msix->table[i]);
                pdev->vpci->msix->table[i] = NULL;
            }
    }

    return 0;
}

struct vpci_msix *msix_find(const struct domain *d, unsigned long addr)
{
    struct vpci_msix *msix;

    ASSERT_PDEV_LIST_IS_READ_LOCKED(d);

    list_for_each_entry ( msix, &d->arch.hvm.msix_tables, next )
    {
        const struct vpci_bar *bars = msix->pdev->vpci->header.bars;
        unsigned int i;

        for ( i = 0; i < ARRAY_SIZE(msix->tables); i++ )
            if ( bars[msix->tables[i] & PCI_MSIX_BIRMASK].enabled &&
                 VMSIX_ADDR_SAME_PAGE(addr, msix->pdev->vpci, i) )
                return msix;
    }

    return NULL;
}

static int cf_check x86_msix_accept(struct vcpu *v, unsigned long addr)
{
    int rc;

    read_lock(&v->domain->pci_lock);
    rc = !!msix_find(v->domain, addr);
    read_unlock(&v->domain->pci_lock);

    return rc;
}

static int cf_check x86_msix_write(struct vcpu *v, unsigned long addr,
    unsigned int len, unsigned long data)
{
    const struct domain *d = v->domain;
    struct vpci_msix *msix = vpci_msix_find(d, addr);

    return vpci_msix_write(msix, addr, len, data);
}

static int cf_check x86_msix_read(struct vcpu *v, unsigned long addr,
    unsigned int len, unsigned long *data)
{
    const struct domain *d = v->domain;
    struct vpci_msix *msix = vpci_msix_find(d, addr);

    return vpci_msix_read(msix, addr, len, data);
}

static const struct hvm_mmio_ops vpci_msix_table_ops = {
    .check = x86_msix_accept,
    .read = x86_msix_read,
    .write = x86_msix_write,
};

void vpci_msix_arch_register(struct vpci_msix *msix, struct domain *d)
{
    if ( list_empty(&d->arch.hvm.msix_tables) )
        register_mmio_handler(d, &vpci_msix_table_ops);

    list_add(&msix->next, &d->arch.hvm.msix_tables);
}
#endif /* CONFIG_HAS_VPCI */
