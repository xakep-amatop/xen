/*
 * xen/arch/arm/gic-v4-its.c
 *
 * ARM Generic Interrupt Controller support v4 version
 * based on xen/arch/arm/gic-v3-its.c and kernel GICv4 driver
 *
 * Copyright (C) 2023 - ARM Ltd
 * Penny Zheng <penny.zheng@arm.com>, ARM Ltd ported to Xen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/delay.h>
#include <xen/err.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <asm/gic_v3_defs.h>
#include <asm/gic_v3_its.h>
#include <asm/gic_v4_its.h>
#include <asm/vgic.h>


/*
 * VPE ID is at most 16 bits.
 * Using a bitmap here limits us to 65536 concurrent VPEs.
 */
#define INVALID_VPEID (~0U)
#define GICR_VPENDBASER_DIRTY_POLL_TIMEOUT_US 100000U
#define GICR_SYNCR_POLL_TIMEOUT_US 100000U

static unsigned long *vpeid_mask;

static spinlock_t vpeid_alloc_lock = SPIN_LOCK_UNLOCKED;

static uint16_t vmovp_seq_num;
static spinlock_t vmovp_lock = SPIN_LOCK_UNLOCKED;

static struct {
    spinlock_t lock;
    struct its_device *dev;
    struct its_vpe **vpes;
    int next_victim;
} vpe_proxy;

void __init gicv4_its_vpeid_allocator_init(void)
{
    /* Allocate space for vpeid_mask based on MAX_VPEID */
    vpeid_mask = xzalloc_array(unsigned long, BITS_TO_LONGS(MAX_VPEID));

    if ( !vpeid_mask )
        panic("Could not allocate VPEID bitmap space\n");
}

static void __iomem *gic_data_rdist_vlpi_base(unsigned int cpu)
{
    /*
     * Each Redistributor defines two 64KB frames in the physical address map.
     * In GICv4, there are two additional 64KB frames.
     * The frames for each Redistributor must be contiguous and must be
     * ordered as follows:
     * 1. RD_base
     * 2. SGI_base
     * 3. VLPI_base
     * 4. Reserved
     */
    return GICD_RDIST_BASE_CPU(cpu) + SZ_128K;
}

static int its_alloc_vpeid(void)
{
    int id;

    spin_lock(&vpeid_alloc_lock);

    id = find_first_zero_bit(vpeid_mask, MAX_VPEID);

    if ( id == MAX_VPEID )
    {
        id = -EBUSY;
        printk(XENLOG_ERR "VPEID pool exhausted\n");
        goto out;
    }

    set_bit(id, vpeid_mask);

out:
    spin_unlock(&vpeid_alloc_lock);

    return id;
}

static void its_free_vpeid(uint32_t vpe_id)
{
    spin_lock(&vpeid_alloc_lock);

    clear_bit(vpe_id, vpeid_mask);

    spin_unlock(&vpeid_alloc_lock);
}

static int its_alloc_vpe_entry(uint32_t vpe_id)
{
    struct host_its *hw_its;
    int ret;

    /*
     * Make sure the L2 tables are allocated on *all* v4 ITSs. We
     * could try and only do it on ITSs corresponding to devices
     * that have interrupts targeted at this VPE, but the
     * complexity becomes crazy.
     */
    list_for_each_entry(hw_its, &host_its_list, entry)
    {
        struct its_baser *baser;

        if ( !hw_its->is_v4 )
            continue;

        baser = its_get_baser(hw_its, GITS_BASER_TYPE_VCPU);
        if ( !baser )
            return -ENODEV;

        ret = its_alloc_table_entry(baser, vpe_id);
        if ( ret )
            return ret;
    }

    return 0;
}

static int its_send_cmd_vsync(struct host_its *its, uint16_t vpeid)
{
    uint64_t cmd[4];

    cmd[0] = GITS_CMD_VSYNC;
    cmd[1] = (uint64_t)vpeid << 32;
    cmd[2] = 0x00;
    cmd[3] = 0x00;

    return its_send_command(its, cmd);
}

static int its_send_cmd_vmapp(struct host_its *its, struct its_vpe *vpe,
                              bool valid)
{
    uint64_t cmd[4];
    uint16_t vpeid = vpe->vpe_id;
    uint64_t vpt_addr;
    int ret;

    cmd[0] = GITS_CMD_VMAPP;
    cmd[1] = (uint64_t)vpeid << 32;
    cmd[2] = valid ? GITS_VALID_BIT : 0;

    /* Unmap command */
    if ( !valid )
        goto out;

    /* Target redistributor */
    cmd[2] |= encode_rdbase(its, vpe->col_idx, 0x0);
    vpt_addr = virt_to_maddr(vpe->vpendtable);
    cmd[3] = (vpt_addr & GENMASK(51, 16)) |
             ((HOST_LPIS_NRBITS - 1) & GENMASK(4, 0));

    /* GICv4.1 adds the VMAPP doorbell pINTID field. */
    if ( gic_has_v4_1_extension() )
        cmd[1] |= (uint64_t)vpe->vpe_db_lpi;

 out:
    ret = its_send_command(its, cmd);

    return ret;
}

static int its_send_cmd_vinvall(struct host_its *its, struct its_vpe *vpe)
{
    uint64_t cmd[4];
    uint16_t vpeid = vpe->vpe_id;

    cmd[0] = GITS_CMD_VINVALL;
    cmd[1] = (uint64_t)vpeid << 32;
    cmd[2] = 0x00;
    cmd[3] = 0x00;

    return its_send_command(its, cmd);
}

static int its_unmap_vpe(struct host_its *its, struct its_vpe *vpe)
{
    int ret;

    ret = its_send_cmd_vmapp(its, vpe, false);
    if ( ret )
        return ret;

    ret = its_send_cmd_vsync(its, vpe->vpe_id);
    if ( ret )
        return ret;

    return gicv3_its_wait_commands(its);
}

static int its_map_vpe(struct host_its *its, struct its_vpe *vpe)
{
    int ret;

    /*
     * VMAPP command maps the vPE to the target RDbase, including an
     * associated virtual LPI Pending table.
     */
    ret = its_send_cmd_vmapp(its, vpe, true);
    if ( ret )
        return ret;

    ret = its_send_cmd_vinvall(its, vpe);
    if ( ret )
        goto rollback;

    ret = its_send_cmd_vsync(its, vpe->vpe_id);
    if ( ret )
        goto rollback;

    ret = gicv3_its_wait_commands(its);
    if ( ret )
        goto rollback;

    return 0;

rollback:
    (void)its_unmap_vpe(its, vpe);

    return ret;
}
static int gicv4_vpe_db_proxy_unmap_locked(struct its_vpe *vpe)
{
    int ret;

    /* Already unmapped? */
    if ( vpe->vpe_proxy_event == -1 )
        return 0;

    ret = its_send_cmd_discard(vpe_proxy.dev->hw_its, vpe_proxy.dev,
                               vpe->vpe_proxy_event);
    if ( ret )
        return ret;
    vpe_proxy.vpes[vpe->vpe_proxy_event] = NULL;

    /*
     * We don't track empty slots at all, so let's move the
     * next_victim pointer to quickly reuse the unmapped slot
     */
    if ( vpe_proxy.vpes[vpe_proxy.next_victim] )
        vpe_proxy.next_victim = vpe->vpe_proxy_event;

    vpe->vpe_proxy_event = -1;

    return 0;
}

static void gicv4_vpe_db_proxy_unmap(struct its_vpe *vpe)
{
    if ( !gic_support_directLPI() )
    {
        unsigned long flags;
        int ret;

        spin_lock_irqsave(&vpe_proxy.lock, flags);
        ret = gicv4_vpe_db_proxy_unmap_locked(vpe);
        spin_unlock_irqrestore(&vpe_proxy.lock, flags);

        if ( ret )
            printk(XENLOG_WARNING
                   "ITS: failed to unmap GICv4 VPE proxy event: %d\n", ret);
    }
}

/*
 * If a GICv4.0 doesn't implement Direct LPIs (which is extremely
 * likely), the only way to perform an invalidate is to use a fake
 * device to issue an INV command, implying that the LPI has first
 * been mapped to some event on that device. Since this is not exactly
 * cheap, we try to keep that mapping around as long as possible, and
 * only issue an UNMAP if we're short on available slots.
 *
 * GICv4.1 mandates that we're able to invalidate by writing to a
 * MMIO register. And most of the time, we don't even have to invalidate
 * vPE doorbell, as the redistributor can be told whether to generate a
 * doorbell or not.
 */
static int gicv4_vpe_db_proxy_map_locked(struct its_vpe *vpe)
{
    unsigned int eventid;
    int ret;

    /* Already mapped? */
    if ( vpe->vpe_proxy_event != -1 )
        return 0;

    /* This slot was already allocated. Kick the other VPE out. */
    eventid = vpe_proxy.next_victim;

    if ( vpe_proxy.vpes[eventid] )
    {
        struct its_vpe *old_vpe = vpe_proxy.vpes[eventid];

        ret = gicv4_vpe_db_proxy_unmap_locked(old_vpe);
        if ( ret )
            return ret;
    }

    ret = its_send_cmd_mapti(vpe_proxy.dev->hw_its, vpe_proxy.dev->host_devid,
                             eventid, vpe->vpe_db_lpi, vpe->col_idx);
    if ( ret )
        return ret;

    /* Commit the software state only after the ITS accepted the mapping. */
    vpe_proxy.vpes[eventid] = vpe;
    vpe->vpe_proxy_event = eventid;
    vpe_proxy.next_victim = (eventid + 1) % vpe_proxy.dev->eventids;

    return 0;
}

int __init gicv4_init_vpe_proxy(void)
{
    struct host_its *hw_its;
    uint32_t devid;

    if ( gic_support_directLPI() )
    {
        printk("ITS: Using DirectLPI for GICv4 VPE invalidation\n");
        return 0;
    }

    /* Any ITS will do, even if not v4. */
    hw_its = list_first_entry_or_null(&host_its_list, struct host_its, entry);
    if ( !hw_its )
    {
        printk(XENLOG_ERR "ITS: No host ITS available for VPE proxy device\n");
        return -ENODEV;
    }

    vpe_proxy.vpes = xzalloc_array(struct its_vpe *, nr_cpu_ids);
    if ( !vpe_proxy.vpes )
    {
        printk(XENLOG_ERR "ITS: Can't allocate GICv4 VPE proxy device array\n");
        return -ENOMEM;
    }

    /* Use the last possible DevID */
    devid = BIT(hw_its->devid_bits, UL) - 1;
    vpe_proxy.dev = its_create_device(hw_its, devid, nr_cpu_ids);
    if ( IS_ERR(vpe_proxy.dev) )
    {
        int ret = PTR_ERR(vpe_proxy.dev);

        printk(XENLOG_ERR "ITS: Can't allocate GICv4 VPE proxy device\n");
        xfree(vpe_proxy.vpes);
        vpe_proxy.vpes = NULL;
        vpe_proxy.dev = NULL;
        return ret;
    }

    spin_lock_init(&vpe_proxy.lock);
    vpe_proxy.next_victim = 0;
    printk(XENLOG_INFO
           "ITS: Allocated DevID %u as GICv4 VPE proxy device\n", devid);

    return 0;
}

static int its_vpe_init(struct its_vpe *vpe)
{
    struct host_its *rollback_its;
    struct page_info *vpendtable;
    struct host_its *hw_its;
    int rc;

    vpe->vpe_id = INVALID_VPEID;

    /* Allocate vpe id */
    rc = its_alloc_vpeid();
    if ( rc < 0 )
        return rc;

    vpe->vpe_id = rc;

    /* Allocate VPT */
    vpendtable = lpi_allocate_pendtable();

    if ( !vpendtable )
    {
        rc = -ENOMEM;
        goto fail;
    }

    vpe->vpendtable = page_to_virt(vpendtable);

    rc = its_alloc_vpe_entry(vpe->vpe_id);
    if ( rc )
        goto fail;

    rwlock_init(&vpe->lock);
    vpe->vpe_proxy_event = -1;
    /*
     * We eagerly inform all the v4 ITS and map vPE to the first
     * possible CPU
     */
    vpe->col_idx = cpumask_first(&cpu_online_map);
    list_for_each_entry(hw_its, &host_its_list, entry)
    {
        if ( !hw_its->is_v4 )
            continue;

        rc = its_map_vpe(hw_its, vpe);
        if ( rc )
            goto fail_unmap;
    }

    vpe->resident = true;

    return 0;

fail_unmap:
    list_for_each_entry(rollback_its, &host_its_list, entry)
    {
        if ( rollback_its == hw_its )
            break;

        if ( rollback_its->is_v4 )
            (void)its_unmap_vpe(rollback_its, vpe);
    }

fail:

    return rc;
}

static void its_vpe_teardown(struct its_vpe *vpe)
static int its_send_cmd_vmovp(struct its_vpe *vpe)
{
    uint16_t vpeid = vpe->vpe_id;
    int ret;
    struct host_its *hw_its;

    if ( !its_list_map )
    {
        uint64_t cmd[4];

        hw_its = list_first_entry(&host_its_list, struct host_its, entry);
        cmd[0] = GITS_CMD_VMOVP;
        cmd[1] = (uint64_t)vpeid << 32;
        cmd[2] = encode_rdbase(hw_its, vpe->col_idx, 0x0);
        cmd[3] = 0x00;

        return its_send_command(hw_its, cmd);
    }

    /*
     * If using the its_list "feature", we need to make sure that all ITSs
     * receive all VMOVP commands in the same order. The only way
     * to guarantee this is to make vmovp a serialization point.
     */
    spin_lock(&vmovp_lock);

    vmovp_seq_num++;

    /* Emit VMOVPs */
    list_for_each_entry(hw_its, &host_its_list, entry)
    {
        uint64_t cmd[4];

        cmd[0] = GITS_CMD_VMOVP | ((uint64_t)vmovp_seq_num << 32);
        cmd[1] = its_list_map | ((uint64_t)vpeid << 32);
        cmd[2] = encode_rdbase(hw_its, vpe->col_idx, 0x0);
        cmd[3] = 0x00;

        ret = its_send_command(hw_its, cmd);
        if ( ret )
        {
            spin_unlock(&vmovp_lock);
            return ret;
        }
    }

    spin_unlock(&vmovp_lock);

    return 0;
}

/* GICR_SYNCR.Busy == 1 until the invalidation completes. */
static int wait_for_syncr(void __iomem *rdbase, const char *what)
{
    unsigned int timeout = GICR_SYNCR_POLL_TIMEOUT_US;

    while ( readl_relaxed(rdbase + GICR_SYNCR) & 1 )
    {
        if ( !timeout-- )
        {
            printk(XENLOG_WARNING
                   "GICv4: timeout waiting for GICR_SYNCR.Busy to clear during %s\n",
                   what);
            return -ETIMEDOUT;
        }

        cpu_relax();
        udelay(1);
    }

    return 0;
}

static int direct_lpi_inv(uint32_t db_lpi, unsigned int cpu)
{
    void __iomem *rdbase;
    uint64_t val;
    int ret;

    /* Register-based LPI invalidation for DB on GICv4.0 */
    val = FIELD_PREP(GICR_INVLPIR_INTID, db_lpi);

    rdbase = per_cpu(rbase, cpu);

    ret = wait_for_syncr(rdbase, "INVLPIR pre-check");
    if ( ret )
        return ret;

    writeq_relaxed(val, rdbase + GICR_INVLPIR);

    return wait_for_syncr(rdbase, "INVLPIR");
}

static void its_vpe_send_inv_db(struct its_vpe *vpe)
{
    if ( gic_support_directLPI() )
    {
        unsigned long flags;
        unsigned int cpu;
        int ret;

        /*
         * Serialize register-based invalidation against VPE moves. The
         * architecture makes overlapping INVLPIR and ITS commands that retarget
         * the same doorbell CONSTRAINED UNPREDICTABLE, and vpe->col_idx is also
         * updated under this lock.
         */
        cpu = vpe_to_cpuid_lock(vpe, &flags);

        /* Target the redistributor this VPE is currently known on */
        ret = direct_lpi_inv(vpe->vpe_db_lpi, cpu);
        vpe_to_cpuid_unlock(vpe, &flags);

        if ( ret )
            printk(XENLOG_WARNING
                   "ITS: failed to invalidate GICv4 VPE doorbell via INVLPIR: %d\n",
                   ret);
    }
    else
    {
        struct its_device *dev = vpe_proxy.dev;
        unsigned long flags;
        int ret;

        spin_lock_irqsave(&vpe_proxy.lock, flags);
        ret = gicv4_vpe_db_proxy_map_locked(vpe);
        if ( !ret )
            ret = its_send_cmd_inv(dev->hw_its, dev->host_devid,
                                   vpe->vpe_proxy_event);
        spin_unlock_irqrestore(&vpe_proxy.lock, flags);

        if ( ret )
            printk(XENLOG_WARNING
                   "ITS: failed to invalidate GICv4 VPE doorbell mapping: %d\n",
                   ret);
    }
}

static void its_vpe_inv_db(struct its_vpe *vpe)
{
    its_vpe_send_inv_db(vpe);
}

void its_vpe_mask_db(struct its_vpe *vpe)
{
    /* Only clear enable bit. */
    lpi_write_config(lpi_data.lpi_property, vpe->vpe_db_lpi, LPI_PROP_ENABLED, 0);
    its_vpe_inv_db(vpe);
}

static void its_vpe_unmask_db(struct its_vpe *vpe)
{
    /* Only set enable bit. */
    lpi_write_config(lpi_data.lpi_property, vpe->vpe_db_lpi, 0, LPI_PROP_ENABLED);
    its_vpe_inv_db(vpe);
}

static void its_vpe_teardown(struct its_vpe *vpe)
{
    struct host_its *hw_its;
    unsigned int order;

    if ( !vpe )
        return;

    order = get_order_from_bytes(max(lpi_data.max_host_lpi_ids / 8, (unsigned long)SZ_64K));
    gicv4_vpe_db_proxy_unmap(vpe);

    if ( vpe->resident )
    {
        list_for_each_entry(hw_its, &host_its_list, entry)
        {
            if ( hw_its->is_v4 )
                (void)its_unmap_vpe(hw_its, vpe);
        }

        vpe->resident = false;
    }

    if ( vpe->vpe_id != INVALID_VPEID )
        its_free_vpeid(vpe->vpe_id);

    if ( vpe->vpendtable )
        free_xenheap_pages(vpe->vpendtable, order);
    xfree(vpe);
}

int vgic_v4_its_vm_init(struct domain *d)
{
    unsigned int nr_vcpus = d->max_vcpus;
    unsigned int nr_db_lpis, nr_chunks, i = 0;
    uint32_t *db_lpi_bases;
    int ret = -ENOMEM;

    if ( !gicv3_its_host_has_its() )
        return 0;

    d->arch.vgic.its_vm = xzalloc(struct its_vm);
    if ( !d->arch.vgic.its_vm )
        return ret;

    d->arch.vgic.its_vm->vpes = xzalloc_array(struct its_vpe *, nr_vcpus);
    if ( !d->arch.vgic.its_vm->vpes )
        goto fail_vpes;
    d->arch.vgic.its_vm->nr_vpes = nr_vcpus;

    d->arch.vgic.its_vm->vproptable = lpi_allocate_vproptable();
    if ( !d->arch.vgic.its_vm->vproptable )
        goto fail_vprop;
    /* Allocate a doorbell interrupt for each VPE. */
    nr_db_lpis = d->arch.vgic.its_vm->nr_vpes;
    nr_chunks = DIV_ROUND_UP(nr_db_lpis, LPI_BLOCK);
    db_lpi_bases = xzalloc_array(uint32_t, nr_chunks);
    if ( !db_lpi_bases )
        goto fail_db_bases;

    do {
        /* Allocate doorbell interrupts in chunks of LPI_BLOCK (=32). */
        ret = gicv3_allocate_host_lpi_block(d, &db_lpi_bases[i]);
        if ( ret )
            goto fail_db;
    } while ( ++i < nr_chunks );

    d->arch.vgic.its_vm->db_lpi_bases = db_lpi_bases;
    d->arch.vgic.its_vm->nr_db_lpis = nr_db_lpis;

    /* Xen assumes all host ITS instances agree on these attributes. */
    d->arch.vgic.its_vm->vpropbaser =
        virt_to_maddr(d->arch.vgic.its_vm->vproptable) & GENMASK(51, 12);
    d->arch.vgic.its_vm->vpropbaser |=
        gicv3_its_get_cacheability() << GICR_VPROPBASER_INNER_CACHEABILITY_SHIFT;
    d->arch.vgic.its_vm->vpropbaser |=
        gicv3_its_get_shareability() << GICR_VPROPBASER_SHAREABILITY_SHIFT;
    d->arch.vgic.its_vm->vpropbaser |=
        GIC_BASER_CACHE_SameAsInner << GICR_VPROPBASER_OUTER_CACHEABILITY_SHIFT;
    d->arch.vgic.its_vm->vpropbaser |=
        (HOST_LPIS_NRBITS - 1) & GICR_VPROPBASER_IDBITS_MASK;

    return 0;

fail_db:
    while ( i != 0 )
        gicv3_free_host_lpi_block(db_lpi_bases[--i]);
    xfree(db_lpi_bases);
fail_db_bases:
    lpi_free_vproptable(d->arch.vgic.its_vm->vproptable);
fail_vprop:
    xfree(d->arch.vgic.its_vm->vpes);
 fail_vpes:
    xfree(d->arch.vgic.its_vm);
    d->arch.vgic.its_vm = NULL;

    return ret;
}

void vgic_v4_free_its_vm(struct domain *d)
{
    struct its_vm *its_vm = d->arch.vgic.its_vm;
    if ( !its_vm )
        return;

    if ( its_vm->db_lpi_bases )
    {
        int nr_chunks = DIV_ROUND_UP(its_vm->nr_db_lpis, LPI_BLOCK);

        while ( --nr_chunks >= 0 )
            gicv3_free_host_lpi_block(its_vm->db_lpi_bases[nr_chunks]);
        xfree(its_vm->db_lpi_bases);
    }

    if ( its_vm->vpes )
        xfree(its_vm->vpes);
    if ( its_vm->vproptable )
        lpi_free_vproptable(its_vm->vproptable);

    xfree(its_vm);
    d->arch.vgic.its_vm = NULL;
}

void vgic_v4_its_vpe_free(struct vcpu *vcpu)
{
    struct its_vm *its_vm = vcpu->domain->arch.vgic.its_vm;
    struct its_vpe *vpe = vcpu->arch.vgic.its_vpe;

    if ( !vpe )
        return;

    if ( its_vm && vcpu->vcpu_id < its_vm->nr_vpes )
        its_vm->vpes[vcpu->vcpu_id] = NULL;

    its_vpe_teardown(vpe);
    vcpu->arch.vgic.its_vpe = NULL;
}

int vgic_v4_its_vpe_init(struct vcpu *vcpu)
{
    int ret;
    struct its_vm *its_vm = vcpu->domain->arch.vgic.its_vm;
    struct its_vpe *vpe;
    unsigned int vcpuid = vcpu->vcpu_id;

    vpe = xzalloc(struct its_vpe);
    if ( !vpe )
        return -ENOMEM;

    vpe->its_vm = its_vm;
    vpe->vpe_db_lpi = its_vm->db_lpi_bases[vcpuid / 32] + (vcpuid % 32);
    vcpu->arch.vgic.its_vpe = vpe;

    ret = its_vpe_init(vpe);
    if ( ret )
    {
        vgic_v4_its_vpe_free(vcpu);
        return ret;
    }

    its_vm->vpes[vcpuid] = vpe;

    /*
     * Sometimes a VLPI gets mapped before the associated VPE becomes
     * resident, so enable the doorbell as soon as initialization succeeds.
     */
    gicv3_lpi_update_host_entry(vpe->vpe_db_lpi, vcpu->domain->domain_id,
                                INVALID_LPI, true, vcpu->vcpu_id);
    its_vpe_unmask_db(vpe);

    return 0;
}

static int its_send_cmd_vmapti(struct host_its *its, struct its_device *dev,
                               const struct its_vlpi_map *map)
{
    uint64_t cmd[4];
    uint32_t deviceid = dev->host_devid;
    uint16_t vpeid = map->vm->vpes[map->vpe_idx]->vpe_id;
    uint32_t vintid = map->vintid;
    uint32_t db_pintid;

    if ( map->db_enabled )
        db_pintid = map->vm->vpes[map->vpe_idx]->vpe_db_lpi;
    else
        db_pintid = INVALID_DBLPI;

    cmd[0] = GITS_CMD_VMAPTI | ((uint64_t)deviceid << 32);
    cmd[1] = map->eventid | ((uint64_t)vpeid << 32);
    cmd[2] = vintid | ((uint64_t)db_pintid << 32);
    cmd[3] = 0x00;

    return its_send_command(its, cmd);
}

static bool pirq_is_forwarded_to_vcpu(struct pending_irq *pirq)
{
    ASSERT(pirq);
    return test_bit(GIC_IRQ_GUEST_FORWARDED, &pirq->status);
}

bool event_is_forwarded_to_vcpu(struct its_device *dev, uint32_t eventid)
{
    struct pending_irq *pirq;

    /* No vlpi maps at all ? */
    if ( !dev->event_map.vlpi_maps)
        return false;

    pirq = dev->event_map.vlpi_maps[eventid].pirq;

    return pirq_is_forwarded_to_vcpu(pirq);
}

static int its_send_cmd_vmovi(struct host_its *its, struct its_vlpi_map *map)
{
    uint64_t cmd[4];
    struct its_device *dev = map->dev;
    uint32_t eventid = map->eventid;
    uint32_t deviceid = dev->host_devid;
    uint16_t vpeid = map->vm->vpes[map->vpe_idx]->vpe_id;
    uint32_t db_pintid;

    if ( map->db_enabled )
        db_pintid = map->vm->vpes[map->vpe_idx]->vpe_db_lpi;
    else
        db_pintid = INVALID_IRQ;

    cmd[0] = GITS_CMD_VMOVI | ((uint64_t)deviceid << 32);
    cmd[1] = eventid | ((uint64_t)vpeid << 32);
    cmd[2] = (map->db_enabled ? 1UL : 0UL) | ((uint64_t)db_pintid << 32);
    cmd[3] = 0x00;

    return its_send_command(its, cmd);
}

static int its_restore_host_mapping(struct its_device *dev, uint32_t eventid)
{
    struct host_its *its = dev->hw_its;
    uint32_t host_lpi;
    int ret;

    host_lpi = dev->host_lpi_blocks[eventid / LPI_BLOCK] +
               (eventid % LPI_BLOCK);

    ret = its_send_cmd_mapti(its, dev->host_devid, eventid, host_lpi, 0);
    if ( ret )
        return ret;

    return its_inv_lpi(its, dev, eventid, 0);
}

static int gicv4_its_vlpi_map(struct its_vlpi_map *map)
{
    struct its_device *dev;
    struct host_its *its;
    struct its_vlpi_map *vlpi_maps;
    uint32_t eventid;
    uint32_t old_vpeid = 0;
    int ret;
    bool forwarded;
    bool new_map_array = false;

    if ( !map )
        return -EINVAL;

    dev = map->dev;
    its = map->dev->hw_its;
    eventid = map->eventid;
    vlpi_maps = dev->event_map.vlpi_maps;

    spin_lock(&dev->event_map.vlpi_lock);

    if ( !dev->event_map.vm )
    {
        vlpi_maps = xzalloc_array(struct its_vlpi_map, dev->event_map.nr_lpis);
        if ( !vlpi_maps )
        {
            ret = -ENOMEM;
            goto out;
        }

        new_map_array = true;
    }
    else if ( dev->event_map.vm != map->vm )
    {
        ret = -EINVAL;
        goto out;
    }

    forwarded = pirq_is_forwarded_to_vcpu(map->pirq);
    if ( forwarded )
        old_vpeid = dev->event_map.vlpi_maps[eventid].vm->vpes[
                    dev->event_map.vlpi_maps[eventid].vpe_idx]->vpe_id;

    if ( forwarded )
    {
        /* Already mapped, move it around */
        ret = its_send_cmd_vmovi(dev->hw_its, map);
        if ( ret )
            goto cleanup;

        dev->event_map.vlpi_maps[eventid] = *map;

        /*
         * ARM spec says that If, after using VMOVI to move an interrupt from
         * vPE A to vPE B, software moves the same interrupt again, a VSYNC
         * command must be issued to vPE A between the moves to ensure correct
         * behavior.
         * So each time we issue VMOVI, we VSYNC the old VPE for good measure.
         */
        ret = its_send_cmd_vsync(dev->hw_its, old_vpeid);
        goto cleanup;
    }
    else
    {
        /* Drop the original physical mapping firstly */
        ret = its_send_cmd_discard(its, dev, eventid);
        if ( ret )
            goto cleanup;

        /* Then install the virtual one */
        ret = its_send_cmd_vmapti(its, dev, map);
        if ( ret )
        {
            int restore_ret;

            restore_ret = its_restore_host_mapping(dev, eventid);
            if ( restore_ret )
                ret = restore_ret;

            goto cleanup;
        }

        if ( new_map_array )
        {
            dev->event_map.vm = map->vm;
            dev->event_map.vlpi_maps = vlpi_maps;
        }

        /* Record the mapping only after the ITS accepted it. */
        dev->event_map.vlpi_maps[eventid] = *map;

        /* Increment the number of VLPIs */
        dev->event_map.nr_vlpis++;
    }

 cleanup:
    if ( ret && new_map_array )
        xfree(vlpi_maps);

 out:
    spin_unlock(&dev->event_map.vlpi_lock);
    return ret;
}

int gicv4_its_vlpi_unmap(struct pending_irq *pirq)
{
    struct its_vlpi_map *map = pirq->vlpi_map;
    struct its_device *dev;
    int ret;
    uint32_t host_lpi;

    if ( !map )
        return -EINVAL;

    dev = map->dev;

    spin_lock(&dev->event_map.vlpi_lock);

    if ( !dev->event_map.vm || !pirq_is_tied_to_hw(pirq) )
    {
        ret = -EINVAL;
        goto out;
    }

    /* Drop the virtual mapping */
    ret = its_send_cmd_discard(dev->hw_its, dev, map->eventid);
    if ( ret )
        goto out;

    /* Restore the physical one */
    host_lpi = dev->host_lpi_blocks[map->eventid / LPI_BLOCK] +
               (map->eventid % LPI_BLOCK);
    /* Map every host LPI to host CPU 0 */
    ret = its_send_cmd_mapti(dev->hw_its, dev->host_devid, map->eventid,
                             host_lpi, 0);
    if ( ret )
        goto out;

    clear_bit(GIC_IRQ_GUEST_FORWARDED, &pirq->status);
    lpi_write_config(lpi_data.lpi_property, host_lpi, 0, LPI_PROP_ENABLED);

    ret = its_inv_lpi(dev->hw_its, dev, map->eventid, 0);
    if ( ret )
        goto out;

    dev->event_map.vlpi_maps[map->eventid] = (struct its_vlpi_map){};
    XFREE(pirq->vlpi_map);
    /*
     * Drop the refcount and make the device available again if
     * this was the last VLPI.
     */
    if ( !--dev->event_map.nr_vlpis )
    {
        dev->event_map.vm = NULL;
        XFREE(dev->event_map.vlpi_maps);
    }

 out:
    spin_unlock(&dev->event_map.vlpi_lock);
    return ret;
}

int gicv4_assign_guest_event(struct domain *d, paddr_t vdoorbell_address,
                             uint32_t vdevid, uint32_t eventid,
                             struct pending_irq *pirq)

{
    int ret = -ENODEV;
    struct its_vm *vm = d->arch.vgic.its_vm;
    struct its_vlpi_map *map;
    struct its_device *dev;

    spin_lock(&d->arch.vgic.its_devices_lock);
    dev = get_its_device(d, vdoorbell_address, vdevid);
    if ( dev && eventid < dev->eventids )
    {
        /* Prepare the vlpi mapping info */
        map = xzalloc(struct its_vlpi_map);
        if ( !map )
        {
            ret = -ENOMEM;
            goto out;
        }
        map->vm = vm;
        map->vintid = pirq->irq;
        map->db_enabled = true;
        map->vpe_idx = pirq->lpi_vcpu_id;
        map->properties = pirq->lpi_priority |
                          (test_bit(GIC_IRQ_GUEST_ENABLED, &pirq->status) ?
                          LPI_PROP_ENABLED : 0);
        map->pirq = pirq;
        map->dev = dev;
        map->eventid = eventid;

        ret = gicv4_its_vlpi_map(map);
        if ( ret )
        {
            xfree(map);
            goto out;
        }

        pirq->vlpi_map = map;
    }

 out:
    spin_unlock(&d->arch.vgic.its_devices_lock);
    return ret;
}

int gicv4_its_vlpi_move(struct pending_irq *pirq, struct vcpu *vcpu)
{
    struct its_vlpi_map *map = pirq->vlpi_map;
    struct its_device *dev;

    if ( !map )
        return -EINVAL;

    dev = map->dev;

    if ( !dev->event_map.vm )
        return -EINVAL;

    map->vpe_idx = vcpu->vcpu_id;
    return gicv4_its_vlpi_map(map);
}

/*
 * There is no real VINV command.
 * We do a normal INV, with a VSYNC instead of a SYNC.
 */
int its_send_cmd_vinv(struct host_its *its, struct its_device *dev,
                      uint32_t eventid)
{
    int ret;
    struct its_vlpi_map *map = &dev->event_map.vlpi_maps[eventid];
    uint16_t vpeid = map->vm->vpes[map->vpe_idx]->vpe_id;

    ret = its_send_cmd_inv(its, dev->host_devid, eventid);
    if ( ret )
        return ret;

    ret = its_send_cmd_vsync(its, vpeid);
    if ( ret )
        return ret;

    return gicv3_its_wait_commands(its);
}

static uint64_t read_vpend_dirty_clean(void __iomem *vlpi_base,
                                       unsigned int count)
{
    uint64_t val;
    bool clean;

    do {
        val = gits_read_vpendbaser(vlpi_base + GICR_VPENDBASER);
        /* Poll GICR_VPENDBASER.Dirty until it reads 0. */
        clean = !(val & GICR_VPENDBASER_Dirty);
        if ( !clean )
        {
            count--;
            cpu_relax();
            udelay(1);
        }
    } while ( !clean && count );

    if ( !clean )
    {
        printk(XENLOG_WARNING "ITS virtual pending table not totally parsed\n");
        val |= GICR_VPENDBASER_PendingLast;
    }

    return val;
}

/*
 * When a vPE is made resident, the GIC starts parsing the virtual pending
 * table to deliver pending interrupts. This takes place asynchronously,
 * and can at times take a long while.
 */
static void its_wait_vpt_parse_complete(void __iomem *vlpi_base)
{
    if ( !gic_support_vptValidDirty() )
        return;

    read_vpend_dirty_clean(vlpi_base, 500);
}

static bool its_clear_vpend_valid(void __iomem *vlpi_base, uint64_t *val)
{
    unsigned int count = GICR_VPENDBASER_DIRTY_POLL_TIMEOUT_US;

    if ( !gits_clear_vpendbaser_valid(vlpi_base + GICR_VPENDBASER) )
        return false;

    *val = read_vpend_dirty_clean(vlpi_base, count);

    return true;
}

static void its_make_vpe_resident(struct its_vpe *vpe, unsigned int cpu)
{
    void __iomem *vlpi_base = gic_data_rdist_vlpi_base(cpu);
    uint64_t val;

    /* Switch in this VM's virtual property table. */
    gits_write_vpropbaser(vpe->its_vm->vpropbaser,
                          vlpi_base + GICR_VPROPBASER);

    /* Switch in this VCPU's VPT. */
    val  = virt_to_maddr(vpe->vpendtable) & GENMASK(51, 16);
    val |= gicv3_its_get_cacheability() << GICR_VPENDBASER_INNER_CACHEABILITY_SHIFT;
    val |= gicv3_its_get_shareability() << GICR_VPENDBASER_SHAREABILITY_SHIFT;
    val |= GIC_BASER_CACHE_SameAsInner << GICR_VPENDBASER_OUTER_CACHEABILITY_SHIFT;
    /*
     * When the GICR_VPENDBASER.Valid bit is written from 0 to 1,
     * this bit is RES1.
     */
    val |= GICR_VPENDBASER_PendingLast;
    val |= vpe->idai ? GICR_VPENDBASER_IDAI : 0;
    val |= GICR_VPENDBASER_Valid;
    gits_write_vpendbaser(val, vlpi_base + GICR_VPENDBASER);

    its_wait_vpt_parse_complete(vlpi_base);
}

static bool its_make_vpe_non_resident(struct its_vpe *vpe, unsigned int cpu)
{
    void __iomem *vlpi_base = gic_data_rdist_vlpi_base(cpu);
    uint64_t val;

    if ( !its_clear_vpend_valid(vlpi_base, &val) )
        return false;

    vpe->idai = val & GICR_VPENDBASER_IDAI;
    write_atomic(&vpe->pending_last, val & GICR_VPENDBASER_PendingLast);

    return true;
}

static int vpe_to_cpuid_lock(struct its_vpe *vpe, unsigned long *flags)
{
    spin_lock_irqsave(&vpe->vpe_lock, *flags);
    return vpe->col_idx;
}

static void vpe_to_cpuid_unlock(struct its_vpe *vpe, unsigned long *flags)
{
    spin_unlock_irqrestore(&vpe->vpe_lock, *flags);
}

static int gicv4_vpe_db_proxy_move(struct its_vpe *vpe, unsigned int from,
                                   unsigned int to)
{
    unsigned long flags;
    int ret;

    if ( gic_support_directLPI() )
    {
        void __iomem *rdbase;

        rdbase = per_cpu(rbase, from);

        ret = wait_for_syncr(rdbase, "CLRLPIR pre-check");
        if ( ret )
        {
            printk(XENLOG_WARNING
                   "ITS: failed to wait for GICR_SYNCR.Busy before CLRLPIR: %d\n",
                   ret);
            return 0;
        }

        /* Clear potential pending state on the old redistributor */
        writeq_relaxed(vpe->vpe_db_lpi, rdbase + GICR_CLRLPIR);

        ret = wait_for_syncr(rdbase, "CLRLPIR");
        if ( ret )
            printk(XENLOG_WARNING
                   "ITS: failed to wait for GICR_SYNCR.Busy after CLRLPIR: %d\n",
                   ret);
        return 0;
    }

    spin_lock_irqsave(&vpe_proxy.lock, flags);

    ret = gicv4_vpe_db_proxy_map_locked(vpe);
    if ( ret )
        goto out;

    /* MOVI instructs the appropriate Redistributor to move the pending state */
    ret = its_send_cmd_movi(vpe_proxy.dev->hw_its, vpe_proxy.dev->host_devid,
                            vpe->vpe_proxy_event, to);
    if ( ret )
        goto out;

    /*
     * ARM spec says that If, after using MOVI to move an interrupt from
     * collection A to collection B, software moves the same interrupt again
     * from collection B to collection C, a SYNC command must be used before
     * the second MOVI for the Redistributor associated with collection A to
     * ensure correct behavior.
     * So each time we issue VMOVI, we VSYNC the old VPE for good measure.
     */
    ret = its_send_cmd_sync(vpe_proxy.dev->hw_its, from);

 out:
    spin_unlock_irqrestore(&vpe_proxy.lock, flags);
    return ret;
}

static int gicv4_vpe_set_affinity(struct vcpu *vcpu)
{
    struct its_vpe *vpe = vcpu->arch.vgic.its_vpe;
    unsigned int from, to = vcpu->processor;
    unsigned long flags;
    int ret = 0;

    /*
     * Changing affinity is mega expensive, so let's be as lazy as
     * we can and only do it if we really have to. Also, if mapped
     * into the proxy device, we need to move the doorbell interrupt
     * to its new location.
     *
     * Another thing is that changing the affinity of a vPE affects
     * *other interrupts* such as all the vLPIs that are routed to
     * this vPE. This means that we must ensure nobody samples
     * vpe->col_idx during the update, hence the lock below which
     * must also be taken on any vLPI handling path that evaluates
     * vpe->col_idx, such as reg-based vLPI invalidation.
     */
    from = vpe_to_cpuid_lock(vpe, &flags);
    if ( from == to )
        goto out;

    vpe->col_idx = to;

    ret = its_send_cmd_vmovp(vpe);
    if ( ret )
        goto out;
    ret = gicv4_vpe_db_proxy_move(vpe, from, to);

 out:
    vpe_to_cpuid_unlock(vpe, &flags);
    return ret;
}

void vgic_v4_load(struct vcpu *vcpu)
{
    struct its_vpe *vpe = vcpu->arch.vgic.its_vpe;

    if ( !vpe )
        return;

    if ( vpe->resident )
        return;

    /*
     * Before making the VPE resident, make sure the redistributor
     * corresponding to our current CPU expects us here
     */
    WARN_ON(gicv4_vpe_set_affinity(vcpu));
    its_vpe_mask_db(vpe);
    its_make_vpe_resident(vpe, vcpu->processor);
    vpe->resident = true;
}

void vgic_v4_put(struct vcpu *vcpu, bool need_db)
{
    struct its_vpe *vpe = vcpu->arch.vgic.its_vpe;

    if ( !vpe )
        return;

    if ( !vpe->resident )
        return;

    if ( !its_make_vpe_non_resident(vpe, vcpu->processor) )
        return;

    if ( need_db )
        /* Enable the doorbell, as the guest is going to block */
        its_vpe_unmask_db(vpe);
    vpe->resident = false;
}

static int its_vlpi_set_doorbell(struct its_vlpi_map *map, bool enable)
{
    if (map->db_enabled == enable)
        return 0;

    map->db_enabled = enable;

    /*
     * Ideally, we'd issue a VMAPTI to set the doorbell to its LPI
     * value or to 1023, depending on the enable bit. But that
     * would be issuing a mapping for an /existing/ DevID+EventID
     * pair, which is UNPREDICTABLE. Instead, let's issue a VMOVI
     * to the /same/ vPE, using this opportunity to adjust the doorbell.
     */
    return its_send_cmd_vmovi(map->dev->hw_its, map);
}

int its_vlpi_prop_update(struct pending_irq *pirq, uint8_t property,
                         bool needs_inv)
{
    struct its_vlpi_map *map;
    unsigned int cpu;
    int ret;

    if ( !pirq->vlpi_map )
        return -EINVAL;

    map = pirq->vlpi_map;

    /* Cache the updated property and update the vproptable. */
    map->properties = property;
    lpi_write_config(map->vm->vproptable, pirq->irq, 0xff, property);

    if ( needs_inv )
    {
        cpu = map->vm->vpes[map->vpe_idx]->col_idx;
        ret = its_inv_lpi(map->dev->hw_its, map->dev, map->eventid, cpu);
        if ( ret )
            return ret;
    }

    return its_vlpi_set_doorbell(map, property & LPI_PROP_ENABLED);
}
