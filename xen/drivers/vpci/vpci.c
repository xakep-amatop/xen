/*
 * Generic functionality for handling accesses to the PCI configuration space
 * from guests.
 *
 * Copyright (C) 2017 Citrix Systems R&D
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/sched.h>
#include <xen/vpci.h>
#include <xen/vmap.h>

/* Internal struct to store the emulated PCI registers. */
struct vpci_register {
    vpci_read_t *read;
    vpci_write_t *write;
    unsigned int size;
    unsigned int offset;
    void *private;
    struct list_head node;
    uint32_t ro_mask;
    uint32_t rw1c_mask;
    uint32_t rsvdp_mask;
    uint32_t rsvdz_mask;
};

#ifdef __XEN__
extern vpci_register_init_t *const __start_vpci_array[];
extern vpci_register_init_t *const __end_vpci_array[];
#define NUM_VPCI_INIT (__end_vpci_array - __start_vpci_array)

#ifdef CONFIG_HAS_VPCI_GUEST_SUPPORT
static int assign_virtual_sbdf(struct pci_dev *pdev)
{
    struct domain *d = pdev->domain;
    unsigned int new_dev_number;

    ASSERT(rw_is_write_locked(&pdev->domain->pci_lock));

    if ( is_hardware_domain(d) )
        return 0;

    /*
     * Each PCI bus supports 32 devices/slots at max or up to 256 when
     * there are multi-function ones which are not yet supported.
     */
    if ( pdev->sbdf.fn )
    {
        gdprintk(XENLOG_ERR, "%pp: only function 0 passthrough supported\n",
                 &pdev->sbdf);
        return -EOPNOTSUPP;
    }
    new_dev_number = find_first_zero_bit(d->vpci_dev_assigned_map,
                                         VPCI_MAX_VIRT_DEV);
    if ( new_dev_number == VPCI_MAX_VIRT_DEV )
        return -ENOSPC;

    __set_bit(new_dev_number, &d->vpci_dev_assigned_map);

    /*
     * Both segment and bus number are 0:
     *  - we emulate a single host bridge for the guest, e.g. segment 0
     *  - with bus 0 the virtual devices are seen as embedded
     *    endpoints behind the root complex
     *
     * TODO: add support for multi-function devices.
     */
    pdev->vpci->guest_sbdf = PCI_SBDF(0, 0, new_dev_number, 0);

    return 0;
}

/*
 * Find the physical device which is mapped to the virtual device
 * and translate virtual SBDF to the physical one.
 */
bool vpci_translate_virtual_device(const struct domain *d, pci_sbdf_t *sbdf)
{
    const struct pci_dev *pdev;

    ASSERT(!is_hardware_domain(d));
    ASSERT(rw_is_locked(&d->pci_lock));

    for_each_pdev ( d, pdev )
    {
        if ( pdev->vpci && (pdev->vpci->guest_sbdf.sbdf == sbdf->sbdf) )
        {
            /* Replace guest SBDF with the physical one. */
            *sbdf = pdev->sbdf;
            return true;
        }
    }

    return false;
}

#endif /* CONFIG_HAS_VPCI_GUEST_SUPPORT */

void vpci_deassign_device(struct pci_dev *pdev)
{
    unsigned int i;

    ASSERT(rw_is_write_locked(&pdev->domain->pci_lock));

    if ( !has_vpci(pdev->domain) || !pdev->vpci )
        return;

#ifdef CONFIG_HAS_VPCI_GUEST_SUPPORT
    if ( pdev->vpci->guest_sbdf.sbdf != ~0 )
        __clear_bit(pdev->vpci->guest_sbdf.dev,
                    &pdev->domain->vpci_dev_assigned_map);
#endif

    spin_lock(&pdev->vpci->lock);
    while ( !list_empty(&pdev->vpci->handlers) )
    {
        struct vpci_register *r = list_first_entry(&pdev->vpci->handlers,
                                                   struct vpci_register,
                                                   node);

        list_del(&r->node);
        xfree(r);
    }
    spin_unlock(&pdev->vpci->lock);
    if ( pdev->vpci->msix )
    {
        list_del(&pdev->vpci->msix->next);
        for ( i = 0; i < ARRAY_SIZE(pdev->vpci->msix->table); i++ )
            if ( pdev->vpci->msix->table[i] )
                iounmap(pdev->vpci->msix->table[i]);
    }

    for ( i = 0; i < ARRAY_SIZE(pdev->vpci->header.bars); i++ )
        rangeset_destroy(pdev->vpci->header.bars[i].mem);

    xfree(pdev->vpci->msix);
    xfree(pdev->vpci->msi);
    xfree(pdev->vpci);
    pdev->vpci = NULL;
}

int vpci_assign_device(struct pci_dev *pdev)
{
    unsigned int i;
    const unsigned long *ro_map;
    int rc = 0;

    ASSERT(rw_is_write_locked(&pdev->domain->pci_lock));

    if ( !has_vpci(pdev->domain) )
        return 0;

    /* We should not get here twice for the same device. */
    ASSERT(!pdev->vpci);

    /* No vPCI for r/o devices. */
    ro_map = pci_get_ro_map(pdev->sbdf.seg);
    if ( ro_map && test_bit(pdev->sbdf.bdf, ro_map) )
        return 0;

    pdev->vpci = xzalloc(struct vpci);
    if ( !pdev->vpci )
        return -ENOMEM;

    INIT_LIST_HEAD(&pdev->vpci->handlers);
    spin_lock_init(&pdev->vpci->lock);

#ifdef CONFIG_HAS_VPCI_GUEST_SUPPORT
    pdev->vpci->guest_sbdf = INVALID_GUEST_SBDF;
    rc = assign_virtual_sbdf(pdev);
    if ( rc )
        goto out;
#endif

    for ( i = 0; i < NUM_VPCI_INIT; i++ )
    {
        rc = __start_vpci_array[i](pdev);
        if ( rc )
            break;
    }

 out: __maybe_unused;
    if ( rc )
        vpci_deassign_device(pdev);

    return rc;
}
#endif /* __XEN__ */

static int vpci_register_cmp(const struct vpci_register *r1,
                             const struct vpci_register *r2)
{
    /* Return 0 if registers overlap. */
    if ( r1->offset < r2->offset + r2->size &&
         r2->offset < r1->offset + r1->size )
        return 0;
    if ( r1->offset < r2->offset )
        return -1;
    if ( r1->offset > r2->offset )
        return 1;

    ASSERT_UNREACHABLE();
    return 0;
}

/* Dummy hooks, writes are ignored, reads return 1's */
static uint32_t cf_check vpci_ignored_read(
    const struct pci_dev *pdev, unsigned int reg, void *data)
{
    return ~(uint32_t)0;
}

static void cf_check vpci_ignored_write(
    const struct pci_dev *pdev, unsigned int reg, uint32_t val, void *data)
{
}

uint32_t cf_check vpci_read_val(
    const struct pci_dev *pdev, unsigned int reg, void *data)
{
    return (uintptr_t)data;
}

uint32_t cf_check vpci_hw_read8(
    const struct pci_dev *pdev, unsigned int reg, void *data)
{
    return pci_conf_read8(pdev->sbdf, reg);
}

uint32_t cf_check vpci_hw_read16(
    const struct pci_dev *pdev, unsigned int reg, void *data)
{
    return pci_conf_read16(pdev->sbdf, reg);
}

uint32_t cf_check vpci_hw_read32(
    const struct pci_dev *pdev, unsigned int reg, void *data)
{
    return pci_conf_read32(pdev->sbdf, reg);
}

void cf_check vpci_hw_write16(
    const struct pci_dev *pdev, unsigned int reg, uint32_t val, void *data)
{
    pci_conf_write16(pdev->sbdf, reg, val);
}

int vpci_add_register_mask(struct vpci *vpci, vpci_read_t *read_handler,
                           vpci_write_t *write_handler, unsigned int offset,
                           unsigned int size, void *data, uint32_t ro_mask,
                           uint32_t rw1c_mask, uint32_t rsvdp_mask,
                           uint32_t rsvdz_mask)
{
    struct list_head *prev;
    struct vpci_register *r;

    /* Some sanity checks. */
    if ( (size != 1 && size != 2 && size != 4) ||
         offset >= PCI_CFG_SPACE_EXP_SIZE || (offset & (size - 1)) ||
         (!read_handler && !write_handler) || (ro_mask & rw1c_mask) ||
         (ro_mask & rsvdp_mask) || (ro_mask & rsvdz_mask) ||
         (rw1c_mask & rsvdp_mask) || (rw1c_mask & rsvdz_mask) ||
         (rsvdp_mask & rsvdz_mask) )
        return -EINVAL;

    if ( size != 4 &&
         ((ro_mask | rw1c_mask | rsvdp_mask | rsvdz_mask) >> (8 * size)) )
        return -EINVAL;

    r = xmalloc(struct vpci_register);
    if ( !r )
        return -ENOMEM;

    r->read = read_handler ?: vpci_ignored_read;
    r->write = write_handler ?: vpci_ignored_write;
    r->size = size;
    r->offset = offset;
    r->private = data;
    r->ro_mask = ro_mask;
    r->rw1c_mask = rw1c_mask;
    r->rsvdp_mask = rsvdp_mask;
    r->rsvdz_mask = rsvdz_mask;

    spin_lock(&vpci->lock);

    /* The list of handlers must be kept sorted at all times. */
    list_for_each ( prev, &vpci->handlers )
    {
        const struct vpci_register *this =
            list_entry(prev, const struct vpci_register, node);
        int cmp = vpci_register_cmp(r, this);

        if ( cmp < 0 )
            break;
        if ( cmp == 0 )
        {
            spin_unlock(&vpci->lock);
            xfree(r);
            return -EEXIST;
        }
    }

    list_add_tail(&r->node, prev);
    spin_unlock(&vpci->lock);

    return 0;
}

int vpci_remove_register(struct vpci *vpci, unsigned int offset,
                         unsigned int size)
{
    const struct vpci_register r = { .offset = offset, .size = size };
    struct vpci_register *rm;

    spin_lock(&vpci->lock);
    list_for_each_entry ( rm, &vpci->handlers, node )
    {
        int cmp = vpci_register_cmp(&r, rm);

        /*
         * NB: do not use a switch so that we can use break to
         * get out of the list loop earlier if required.
         */
        if ( !cmp && rm->offset == offset && rm->size == size )
        {
            list_del(&rm->node);
            spin_unlock(&vpci->lock);
            xfree(rm);
            return 0;
        }
        if ( cmp <= 0 )
            break;
    }
    spin_unlock(&vpci->lock);

    return -ENOENT;
}

/* Wrappers for performing reads/writes to the underlying hardware. */
static uint32_t vpci_read_hw(pci_sbdf_t sbdf, unsigned int reg,
                             unsigned int size)
{
    uint32_t data;

    /* Guest domains are not allowed to read real hardware. */
    if ( !is_hardware_domain(current->domain) )
        return ~(uint32_t)0;

    switch ( size )
    {
    case 4:
        data = pci_conf_read32(sbdf, reg);
        break;

    case 3:
        /*
         * This is possible because a 4byte read can have 1byte trapped and
         * the rest passed-through.
         */
        if ( reg & 1 )
        {
            data = pci_conf_read8(sbdf, reg);
            data |= pci_conf_read16(sbdf, reg + 1) << 8;
        }
        else
        {
            data = pci_conf_read16(sbdf, reg);
            data |= pci_conf_read8(sbdf, reg + 2) << 16;
        }
        break;

    case 2:
        data = pci_conf_read16(sbdf, reg);
        break;

    case 1:
        data = pci_conf_read8(sbdf, reg);
        break;

    default:
        ASSERT_UNREACHABLE();
        data = ~(uint32_t)0;
        break;
    }

    return data;
}

static void vpci_write_hw(pci_sbdf_t sbdf, unsigned int reg, unsigned int size,
                          uint32_t data)
{
    /* Guest domains are not allowed to write real hardware. */
    if ( !is_hardware_domain(current->domain) )
        return;

    switch ( size )
    {
    case 4:
        pci_conf_write32(sbdf, reg, data);
        break;

    case 3:
        /*
         * This is possible because a 4byte write can have 1byte trapped and
         * the rest passed-through.
         */
        if ( reg & 1 )
        {
            pci_conf_write8(sbdf, reg, data);
            pci_conf_write16(sbdf, reg + 1, data >> 8);
        }
        else
        {
            pci_conf_write16(sbdf, reg, data);
            pci_conf_write8(sbdf, reg + 2, data >> 16);
        }
        break;

    case 2:
        pci_conf_write16(sbdf, reg, data);
        break;

    case 1:
        pci_conf_write8(sbdf, reg, data);
        break;

    default:
        ASSERT_UNREACHABLE();
        break;
    }
}

/*
 * Merge new data into a partial result.
 *
 * Copy the value found in 'new' from [0, size) left shifted by
 * 'offset' into 'data'. Note that both 'size' and 'offset' are
 * in byte units.
 */
static uint32_t merge_result(uint32_t data, uint32_t new, unsigned int size,
                             unsigned int offset)
{
    uint32_t mask = 0xffffffffU >> (32 - 8 * size);

    return (data & ~(mask << (offset * 8))) | ((new & mask) << (offset * 8));
}

uint32_t vpci_read(pci_sbdf_t sbdf, unsigned int reg, unsigned int size)
{
    struct domain *d = current->domain;
    const struct pci_dev *pdev;
    const struct vpci_register *r;
    unsigned int data_offset = 0;
    uint32_t data = ~(uint32_t)0;

    if ( !size )
    {
        ASSERT_UNREACHABLE();
        return data;
    }

    /*
     * Find the PCI dev matching the address, which for hwdom also requires
     * consulting DomXEN.  Passthrough everything that's not trapped.
     * If this is hwdom and the device is assigned to DomXEN, acquiring hwdom's
     * pci_lock is sufficient.
     */
    read_lock(&d->pci_lock);
    pdev = pci_get_pdev(d, sbdf);
    if ( !pdev && is_hardware_domain(d) )
        pdev = pci_get_pdev(dom_xen, sbdf);
    if ( !pdev || !pdev->vpci )
    {
        read_unlock(&d->pci_lock);
        return vpci_read_hw(sbdf, reg, size);
    }

    spin_lock(&pdev->vpci->lock);

    /* Read from the hardware or the emulated register handlers. */
    list_for_each_entry ( r, &pdev->vpci->handlers, node )
    {
        const struct vpci_register emu = {
            .offset = reg + data_offset,
            .size = size - data_offset
        };
        int cmp = vpci_register_cmp(&emu, r);
        uint32_t val;
        unsigned int read_size;

        if ( cmp < 0 )
            break;
        if ( cmp > 0 )
            continue;

        if ( emu.offset < r->offset )
        {
            /* Heading gap, read partial content from hardware. */
            read_size = r->offset - emu.offset;
            val = vpci_read_hw(sbdf, emu.offset, read_size);
            data = merge_result(data, val, read_size, data_offset);
            data_offset += read_size;
        }

        val = r->read(pdev, r->offset, r->private);
        val &= ~(r->rsvdp_mask | r->rsvdz_mask);

        /* Check if the read is in the middle of a register. */
        if ( r->offset < emu.offset )
            val >>= (emu.offset - r->offset) * 8;

        /* Find the intersection size between the two sets. */
        read_size = min(emu.offset + emu.size, r->offset + r->size) -
                    max(emu.offset, r->offset);
        /* Merge the emulated data into the native read value. */
        data = merge_result(data, val, read_size, data_offset);
        data_offset += read_size;
        if ( data_offset == size )
            break;
        ASSERT(data_offset < size);
    }
    spin_unlock(&pdev->vpci->lock);
    read_unlock(&d->pci_lock);

    if ( data_offset < size )
    {
        /* Tailing gap, read the remaining. */
        uint32_t tmp_data = vpci_read_hw(sbdf, reg + data_offset,
                                         size - data_offset);

        data = merge_result(data, tmp_data, size - data_offset, data_offset);
    }

    return data & (0xffffffffU >> (32 - 8 * size));
}

/*
 * Perform a maybe partial write to a register.
 */
static void vpci_write_helper(const struct pci_dev *pdev,
                              const struct vpci_register *r, unsigned int size,
                              unsigned int offset, uint32_t data)
{
    uint32_t curval = 0;
    uint32_t preserved_mask = r->ro_mask | r->rsvdp_mask;

    ASSERT(size <= r->size);

    if ( (size != r->size) || preserved_mask )
    {
        curval = r->read(pdev, r->offset, r->private);
        curval &= ~r->rw1c_mask;
        data = merge_result(curval, data, size, offset);
    }

    data &= ~(preserved_mask | r->rsvdz_mask);
    data |= curval & preserved_mask;

    r->write(pdev, r->offset, data & (0xffffffffU >> (32 - 8 * r->size)),
             r->private);
}

void vpci_write(pci_sbdf_t sbdf, unsigned int reg, unsigned int size,
                uint32_t data)
{
    struct domain *d = current->domain;
    const struct pci_dev *pdev;
    const struct vpci_register *r;
    unsigned int data_offset = 0;

    if ( !size )
    {
        ASSERT_UNREACHABLE();
        return;
    }

    /*
     * Find the PCI dev matching the address, which for hwdom also requires
     * consulting DomXEN.  Passthrough everything that's not trapped.
     * If this is hwdom and the device is assigned to DomXEN, acquiring hwdom's
     * pci_lock is sufficient.
     *
     * TODO: We need to take pci_locks in exclusive mode only if we
     * are modifying BARs, so there is a room for improvement.
     */
    write_lock(&d->pci_lock);
    pdev = pci_get_pdev(d, sbdf);
    if ( !pdev && is_hardware_domain(d) )
        pdev = pci_get_pdev(dom_xen, sbdf);
    if ( !pdev || !pdev->vpci )
    {
        /* Ignore writes to read-only devices, which have no ->vpci. */
        const unsigned long *ro_map = pci_get_ro_map(sbdf.seg);

        write_unlock(&d->pci_lock);

        if ( !ro_map || !test_bit(sbdf.bdf, ro_map) )
            vpci_write_hw(sbdf, reg, size, data);
        return;
    }

    spin_lock(&pdev->vpci->lock);

    /* Write the value to the hardware or emulated registers. */
    list_for_each_entry ( r, &pdev->vpci->handlers, node )
    {
        const struct vpci_register emu = {
            .offset = reg + data_offset,
            .size = size - data_offset
        };
        int cmp = vpci_register_cmp(&emu, r);
        unsigned int write_size;

        if ( cmp < 0 )
            break;
        if ( cmp > 0 )
            continue;

        if ( emu.offset < r->offset )
        {
            /* Heading gap, write partial content to hardware. */
            vpci_write_hw(sbdf, emu.offset, r->offset - emu.offset,
                          data >> (data_offset * 8));
            data_offset += r->offset - emu.offset;
        }

        /* Find the intersection size between the two sets. */
        write_size = min(emu.offset + emu.size, r->offset + r->size) -
                     max(emu.offset, r->offset);
        vpci_write_helper(pdev, r, write_size, reg + data_offset - r->offset,
                          data >> (data_offset * 8));
        data_offset += write_size;
        if ( data_offset == size )
            break;
        ASSERT(data_offset < size);
    }
    spin_unlock(&pdev->vpci->lock);
    write_unlock(&d->pci_lock);

    if ( data_offset < size )
        /* Tailing gap, write the remaining. */
        vpci_write_hw(sbdf, reg + data_offset, size - data_offset,
                      data >> (data_offset * 8));
}

/* Helper function to check an access size and alignment on vpci space. */
bool vpci_access_allowed(unsigned int reg, unsigned int len)
{
    /* Check access size. */
    if ( len != 1 && len != 2 && len != 4 && len != 8 )
        return false;

#ifndef CONFIG_64BIT
    /* Prevent 64bit accesses on 32bit */
    if ( len == 8 )
        return false;
#endif

    /* Check that access is size aligned. */
    if ( (reg & (len - 1)) )
        return false;

    return true;
}

bool vpci_ecam_write(pci_sbdf_t sbdf, unsigned int reg, unsigned int len,
                         unsigned long data)
{
    if ( !vpci_access_allowed(reg, len) ||
         (reg + len) > PCI_CFG_SPACE_EXP_SIZE )
        return false;

    vpci_write(sbdf, reg, min(4u, len), data);
#ifdef CONFIG_64BIT
    if ( len == 8 )
        vpci_write(sbdf, reg + 4, 4, data >> 32);
#endif

    return true;
}

bool vpci_ecam_read(pci_sbdf_t sbdf, unsigned int reg, unsigned int len,
                        unsigned long *data)
{
    if ( !vpci_access_allowed(reg, len) ||
         (reg + len) > PCI_CFG_SPACE_EXP_SIZE )
        return false;

    /*
     * According to the PCIe 3.1A specification:
     *  - Configuration Reads and Writes must usually be DWORD or smaller
     *    in size.
     *  - Because Root Complex implementations are not required to support
     *    accesses to a RCRB that cross DW boundaries [...] software
     *    should take care not to cause the generation of such accesses
     *    when accessing a RCRB unless the Root Complex will support the
     *    access.
     *  Xen however supports 8byte accesses by splitting them into two
     *  4byte accesses.
     */
    *data = vpci_read(sbdf, reg, min(4u, len));
#ifdef CONFIG_64BIT
    if ( len == 8 )
        *data |= (uint64_t)vpci_read(sbdf, reg + 4, 4) << 32;
#endif

    return true;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
