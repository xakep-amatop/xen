/*
 * Copyright (C) 2007 Advanced Micro Devices, Inc.
 * Author: Leo Duran <leo.duran@amd.com>
 * Author: Wei Wang <wei.wang2@amd.com> - adapted to xen
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/acpi.h>
#include <xen/param.h>

#include <asm/io_apic.h>

#include <acpi/actables.h>

#include "iommu.h"

/* Some helper structures, particularly to deal with ranges. */

struct acpi_ivhd_device_range {
   struct acpi_ivrs_device4 start;
   struct acpi_ivrs_device4 end;
};

struct acpi_ivhd_device_alias_range {
   struct acpi_ivrs_device8a alias;
   struct acpi_ivrs_device4 end;
};

struct acpi_ivhd_device_extended_range {
   struct acpi_ivrs_device8b extended;
   struct acpi_ivrs_device4 end;
};

union acpi_ivhd_device {
   struct acpi_ivrs_de_header header;
   struct acpi_ivrs_device4 select;
   struct acpi_ivhd_device_range range;
   struct acpi_ivrs_device8a alias;
   struct acpi_ivhd_device_alias_range alias_range;
   struct acpi_ivrs_device8b extended;
   struct acpi_ivhd_device_extended_range extended_range;
   struct acpi_ivrs_device8c special;
};

static void __init add_ivrs_mapping_entry(
    uint16_t bdf, uint16_t alias_id, uint8_t flags, unsigned int ext_flags,
    bool alloc_irt, struct amd_iommu *iommu)
{
    struct ivrs_mappings *ivrs_mappings = get_ivrs_mappings(iommu->sbdf.seg);

    ASSERT( ivrs_mappings != NULL );

    /* setup requestor id */
    ivrs_mappings[bdf].dte_requestor_id = alias_id;

    /* override flags for range of devices */
    ivrs_mappings[bdf].block_ats = ext_flags & ACPI_IVHD_ATS_DISABLED;
    ivrs_mappings[bdf].device_flags = flags;

    /* Don't map an IOMMU by itself. */
    if ( iommu->sbdf.bdf == bdf )
        return;

    /* Allocate interrupt remapping table if needed. */
    if ( iommu_intremap && !ivrs_mappings[alias_id].intremap_table )
    {
        if ( !amd_iommu_perdev_intremap )
        {
            if ( !shared_intremap_table )
                shared_intremap_table = amd_iommu_alloc_intremap_table(
                    iommu, &shared_intremap_inuse, 0);

            if ( !shared_intremap_table )
                panic("No memory for shared IRT\n");

            ivrs_mappings[alias_id].intremap_table = shared_intremap_table;
            ivrs_mappings[alias_id].intremap_inuse = shared_intremap_inuse;
        }
        else if ( alloc_irt )
        {
            ivrs_mappings[alias_id].intremap_table =
                amd_iommu_alloc_intremap_table(
                    iommu, &ivrs_mappings[alias_id].intremap_inuse, 0);

            if ( !ivrs_mappings[alias_id].intremap_table )
                panic("No memory for %pp's IRT\n",
                      &PCI_SBDF(iommu->sbdf.seg, alias_id));
        }
    }

    ivrs_mappings[alias_id].valid = true;

    /* Assign IOMMU hardware. */
    ivrs_mappings[bdf].iommu = iommu;
}

static struct amd_iommu * __init find_iommu_from_bdf_cap(
    u16 seg, u16 bdf, u16 cap_offset)
{
    struct amd_iommu *iommu;

    for_each_amd_iommu ( iommu )
        if ( (iommu->sbdf.sbdf == PCI_SBDF(seg, bdf).sbdf) &&
             (iommu->cap_offset == cap_offset) )
            return iommu;

    return NULL;
}

static int __init reserve_iommu_exclusion_range(
    struct amd_iommu *iommu, paddr_t base, paddr_t limit, bool all)
{
    /* need to extend exclusion range? */
    if ( iommu->exclusion_enable )
    {
        if ( iommu->exclusion_limit + PAGE_SIZE < base ||
             limit + PAGE_SIZE < iommu->exclusion_base ||
             iommu->exclusion_allow_all != all )
            return -EBUSY;

        if ( iommu->exclusion_base < base )
            base = iommu->exclusion_base;
        if ( iommu->exclusion_limit > limit )
            limit = iommu->exclusion_limit;
    }

    iommu->exclusion_enable = IOMMU_CONTROL_ENABLED;
    iommu->exclusion_allow_all = all;
    iommu->exclusion_base = base;
    iommu->exclusion_limit = limit;

    return 0;
}

static int __init reserve_unity_map_for_device(
    uint16_t seg, uint16_t bdf, unsigned long base,
    unsigned long length, bool iw, bool ir, bool global)
{
    struct ivrs_mappings *ivrs_mappings = get_ivrs_mappings(seg);
    struct ivrs_unity_map *unity_map = ivrs_mappings[bdf].unity_map;
    int paging_mode = amd_iommu_get_paging_mode(PFN_UP(base + length));

    if ( paging_mode < 0 )
        return paging_mode;

    /* Check for overlaps. */
    for ( ; unity_map; unity_map = unity_map->next )
    {
        /*
         * Exact matches are okay. This can in particular happen when
         * register_range_for_device() calls here twice for the same
         * (s,b,d,f).
         */
        if ( base == unity_map->addr && length == unity_map->length &&
             ir == unity_map->read && iw == unity_map->write )
        {
            if ( global )
                unity_map->global = true;
            return 0;
        }

        if ( unity_map->addr + unity_map->length > base &&
             base + length > unity_map->addr )
        {
            AMD_IOMMU_ERROR("IVMD: overlap [%lx,%lx) vs [%lx,%lx)\n",
                            base, base + length, unity_map->addr,
                            unity_map->addr + unity_map->length);
            return -EPERM;
        }
    }

    /* Populate and insert a new unity map. */
    unity_map = xmalloc(struct ivrs_unity_map);
    if ( !unity_map )
        return -ENOMEM;

    unity_map->read = ir;
    unity_map->write = iw;
    unity_map->global = global;
    unity_map->addr = base;
    unity_map->length = length;
    unity_map->next = ivrs_mappings[bdf].unity_map;
    ivrs_mappings[bdf].unity_map = unity_map;

    if ( paging_mode > amd_iommu_min_paging_mode )
        amd_iommu_min_paging_mode = paging_mode;

    return 0;
}

static int __init register_range_for_all_devices(
    paddr_t base, paddr_t limit, bool iw, bool ir, bool exclusion)
{
    int seg = 0; /* XXX */
    struct amd_iommu *iommu;
    int rc = 0;

    /* is part of exclusion range inside of IOMMU virtual address space? */
    /* note: 'limit' parameter is assumed to be page-aligned */
    if ( exclusion )
    {
        for_each_amd_iommu( iommu )
        {
            int ret = reserve_iommu_exclusion_range(iommu, base, limit,
                                                    true /* all */);

            if ( ret && !rc )
                rc = ret;
        }
    }

    if ( !exclusion || rc )
    {
        paddr_t length = limit + PAGE_SIZE - base;
        unsigned int bdf;

        /* reserve r/w unity-mapped page entries for devices */
        for ( bdf = rc = 0; !rc && bdf < ivrs_bdf_entries; bdf++ )
            rc = reserve_unity_map_for_device(seg, bdf, base, length, iw, ir,
                                              true);
    }

    return rc;
}

static int __init register_range_for_device(
    unsigned int bdf, paddr_t base, paddr_t limit,
    bool iw, bool ir, bool exclusion)
{
    pci_sbdf_t sbdf = { .seg = 0 /* XXX */, .bdf = bdf };
    struct ivrs_mappings *ivrs_mappings = get_ivrs_mappings(sbdf.seg);
    struct amd_iommu *iommu;
    u16 req;
    int rc = 0;

    iommu = find_iommu_for_device(sbdf);
    if ( !iommu )
    {
        AMD_IOMMU_WARN("IVMD: no IOMMU for device %pp - ignoring constrain\n",
                       &sbdf);
        return 0;
    }
    req = ivrs_mappings[bdf].dte_requestor_id;

    /* note: 'limit' parameter is assumed to be page-aligned */
    if ( exclusion )
        rc = reserve_iommu_exclusion_range(iommu, base, limit,
                                           false /* all */);
    if ( !exclusion || rc )
    {
        paddr_t length = limit + PAGE_SIZE - base;

        /* reserve unity-mapped page entries for device */
        rc = reserve_unity_map_for_device(sbdf.seg, bdf, base, length, iw, ir,
                                          false) ?:
             reserve_unity_map_for_device(sbdf.seg, req, base, length, iw, ir,
                                          false);
    }
    else
    {
        ivrs_mappings[bdf].dte_allow_exclusion = true;
        ivrs_mappings[req].dte_allow_exclusion = true;
    }

    return rc;
}

static int __init register_range_for_iommu_devices(
    struct amd_iommu *iommu, paddr_t base, paddr_t limit,
    bool iw, bool ir, bool exclusion)
{
    /* note: 'limit' parameter is assumed to be page-aligned */
    paddr_t length = limit + PAGE_SIZE - base;
    unsigned int bdf;
    u16 req;
    int rc;

    if ( exclusion )
    {
        rc = reserve_iommu_exclusion_range(iommu, base, limit, true /* all */);
        if ( !rc )
            return 0;
    }

    /* reserve unity-mapped page entries for devices */
    for ( bdf = rc = 0; !rc && bdf < ivrs_bdf_entries; bdf++ )
    {
        if ( iommu != find_iommu_for_device(PCI_SBDF(iommu->sbdf.seg, bdf)) )
            continue;

        req = get_ivrs_mappings(iommu->sbdf.seg)[bdf].dte_requestor_id;
        rc = reserve_unity_map_for_device(iommu->sbdf.seg, bdf, base, length,
                                          iw, ir, false) ?:
             reserve_unity_map_for_device(iommu->sbdf.seg, req, base, length,
                                          iw, ir, false);
    }

    return rc;
}

static int __init parse_ivmd_device_select(
    const struct acpi_ivrs_memory *ivmd_block,
    paddr_t base, paddr_t limit, bool iw, bool ir, bool exclusion)
{
    u16 bdf;

    bdf = ivmd_block->header.device_id;
    if ( bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVMD: invalid Dev_Id %#x\n", bdf);
        return -ENODEV;
    }

    return register_range_for_device(bdf, base, limit, iw, ir, exclusion);
}

static int __init parse_ivmd_device_range(
    const struct acpi_ivrs_memory *ivmd_block,
    paddr_t base, paddr_t limit, bool iw, bool ir, bool exclusion)
{
    unsigned int first_bdf, last_bdf, bdf;
    int error;

    first_bdf = ivmd_block->header.device_id;
    if ( first_bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVMD: invalid Range_First Dev_Id %#x\n", first_bdf);
        return -ENODEV;
    }

    last_bdf = ivmd_block->aux_data;
    if ( (last_bdf >= ivrs_bdf_entries) || (last_bdf <= first_bdf) )
    {
        AMD_IOMMU_ERROR("IVMD: invalid Range_Last Dev_Id %#x\n", last_bdf);
        return -ENODEV;
    }

    for ( bdf = first_bdf, error = 0; (bdf <= last_bdf) && !error; bdf++ )
        error = register_range_for_device(
            bdf, base, limit, iw, ir, exclusion);

    return error;
}

static int __init parse_ivmd_device_iommu(
    const struct acpi_ivrs_memory *ivmd_block,
    paddr_t base, paddr_t limit, bool iw, bool ir, bool exclusion)
{
    int seg = 0; /* XXX */
    struct amd_iommu *iommu;

    /* find target IOMMU */
    iommu = find_iommu_from_bdf_cap(seg, ivmd_block->header.device_id,
                                    ivmd_block->aux_data);
    if ( !iommu )
    {
        AMD_IOMMU_ERROR("IVMD: no IOMMU for Dev_Id %#x Cap %#x\n",
                        ivmd_block->header.device_id, ivmd_block->aux_data);
        return -ENODEV;
    }

    return register_range_for_iommu_devices(
        iommu, base, limit, iw, ir, exclusion);
}

static int __init parse_ivmd_block(const struct acpi_ivrs_memory *ivmd_block)
{
    unsigned long start_addr, mem_length, base, limit;
    unsigned int addr_bits;
    bool iw = true, ir = true, exclusion = false;

    if ( ivmd_block->header.length < sizeof(*ivmd_block) )
    {
        AMD_IOMMU_ERROR("IVMD: invalid block length\n");
        return -ENODEV;
    }

    start_addr = (unsigned long)ivmd_block->start_address;
    mem_length = (unsigned long)ivmd_block->memory_length;
    base = start_addr & PAGE_MASK;
    limit = (start_addr + mem_length - 1) & PAGE_MASK;

    AMD_IOMMU_DEBUG("IVMD Block: type %#x phys %#lx len %#lx\n",
                    ivmd_block->header.type, start_addr, mem_length);

    addr_bits = min(MASK_EXTR(amd_iommu_acpi_info, ACPI_IVRS_PHYSICAL_SIZE),
                    MASK_EXTR(amd_iommu_acpi_info, ACPI_IVRS_VIRTUAL_SIZE));
    if ( amd_iommu_get_paging_mode(PFN_UP(start_addr + mem_length)) < 0 ||
         (addr_bits < BITS_PER_LONG &&
          ((start_addr + mem_length - 1) >> addr_bits)) )
    {
        AMD_IOMMU_WARN("IVMD: [%lx,%lx) is not IOMMU addressable\n",
                       start_addr, start_addr + mem_length);
        return 0;
    }

    if ( !iommu_unity_region_ok("IVMD", maddr_to_mfn(base),
                                maddr_to_mfn(limit)) )
        return -EIO;

    if ( ivmd_block->header.flags & ACPI_IVMD_EXCLUSION_RANGE )
        exclusion = true;
    else if ( ivmd_block->header.flags & ACPI_IVMD_UNITY )
    {
        iw = ivmd_block->header.flags & ACPI_IVMD_READ;
        ir = ivmd_block->header.flags & ACPI_IVMD_WRITE;
    }
    else
    {
        AMD_IOMMU_ERROR("IVMD: invalid flag field\n");
        return -ENODEV;
    }

    switch( ivmd_block->header.type )
    {
    case ACPI_IVRS_TYPE_MEMORY_ALL:
        return register_range_for_all_devices(
            base, limit, iw, ir, exclusion);

    case ACPI_IVRS_TYPE_MEMORY_ONE:
        return parse_ivmd_device_select(ivmd_block, base, limit,
                                        iw, ir, exclusion);

    case ACPI_IVRS_TYPE_MEMORY_RANGE:
        return parse_ivmd_device_range(ivmd_block, base, limit,
                                       iw, ir, exclusion);

    case ACPI_IVRS_TYPE_MEMORY_IOMMU:
        return parse_ivmd_device_iommu(ivmd_block, base, limit,
                                       iw, ir, exclusion);

    default:
        AMD_IOMMU_ERROR("IVMD: unknown block type %#x\n",
                        ivmd_block->header.type);
        return -ENODEV;
    }
}

static u16 __init parse_ivhd_device_padding(
    u16 pad_length, u16 header_length, u16 block_length)
{
    if ( header_length < (block_length + pad_length) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry length\n");
        return 0;
    }

    return pad_length;
}

static u16 __init parse_ivhd_device_select(
    const struct acpi_ivrs_device4 *select, struct amd_iommu *iommu)
{
    u16 bdf;

    bdf = select->header.id;
    if ( bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry Dev_Id %#x\n", bdf);
        return 0;
    }

    add_ivrs_mapping_entry(bdf, bdf, select->header.data_setting, 0, false,
                           iommu);

    return sizeof(*select);
}

static u16 __init parse_ivhd_device_range(
    const struct acpi_ivhd_device_range *range,
    u16 header_length, u16 block_length, struct amd_iommu *iommu)
{
    unsigned int dev_length, first_bdf, last_bdf, bdf;

    dev_length = sizeof(*range);
    if ( header_length < (block_length + dev_length) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry length\n");
        return 0;
    }

    if ( range->end.header.type != ACPI_IVRS_TYPE_END )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: End_Type %#x\n",
                        range->end.header.type);
        return 0;
    }

    first_bdf = range->start.header.id;
    if ( first_bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: First Dev_Id %#x\n", first_bdf);
        return 0;
    }

    last_bdf = range->end.header.id;
    if ( (last_bdf >= ivrs_bdf_entries) || (last_bdf <= first_bdf) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: Last Dev_Id %#x\n", last_bdf);
        return 0;
    }

    AMD_IOMMU_DEBUG(" Dev_Id Range: %#x -> %#x\n", first_bdf, last_bdf);

    for ( bdf = first_bdf; bdf <= last_bdf; bdf++ )
        add_ivrs_mapping_entry(bdf, bdf, range->start.header.data_setting, 0,
                               false, iommu);

    return dev_length;
}

static u16 __init parse_ivhd_device_alias(
    const struct acpi_ivrs_device8a *alias,
    u16 header_length, u16 block_length, struct amd_iommu *iommu)
{
    u16 dev_length, alias_id, bdf;

    dev_length = sizeof(*alias);
    if ( header_length < (block_length + dev_length) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry length\n");
        return 0;
    }

    bdf = alias->header.id;
    if ( bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry Dev_Id %#x\n", bdf);
        return 0;
    }

    alias_id = alias->used_id;
    if ( alias_id >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Alias Dev_Id %#x\n", alias_id);
        return 0;
    }

    AMD_IOMMU_DEBUG(" Dev_Id Alias: %#x\n", alias_id);

    add_ivrs_mapping_entry(bdf, alias_id, alias->header.data_setting, 0, true,
                           iommu);

    return dev_length;
}

static u16 __init parse_ivhd_device_alias_range(
    const struct acpi_ivhd_device_alias_range *range,
    u16 header_length, u16 block_length, struct amd_iommu *iommu)
{

    unsigned int dev_length, first_bdf, last_bdf, alias_id, bdf;

    dev_length = sizeof(*range);
    if ( header_length < (block_length + dev_length) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry length\n");
        return 0;
    }

    if ( range->end.header.type != ACPI_IVRS_TYPE_END )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: End_Type %#x\n",
                        range->end.header.type);
        return 0;
    }

    first_bdf = range->alias.header.id;
    if ( first_bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: First Dev_Id %#x\n", first_bdf);
        return 0;
    }

    last_bdf = range->end.header.id;
    if ( last_bdf >= ivrs_bdf_entries || last_bdf <= first_bdf )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: Last Dev_Id %#x\n", last_bdf);
        return 0;
    }

    alias_id = range->alias.used_id;
    if ( alias_id >= ivrs_bdf_entries )
    {
        AMD_IOMMU_DEBUG("IVHD Error: Invalid Alias Dev_Id %#x\n", alias_id);
        return 0;
    }

    AMD_IOMMU_DEBUG(" Dev_Id Range: %#x -> %#x alias %#x\n",
                    first_bdf, last_bdf, alias_id);

    for ( bdf = first_bdf; bdf <= last_bdf; bdf++ )
        add_ivrs_mapping_entry(bdf, alias_id, range->alias.header.data_setting,
                               0, true, iommu);

    return dev_length;
}

static u16 __init parse_ivhd_device_extended(
    const struct acpi_ivrs_device8b *ext,
    u16 header_length, u16 block_length, struct amd_iommu *iommu)
{
    u16 dev_length, bdf;

    dev_length = sizeof(*ext);
    if ( header_length < (block_length + dev_length) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry length\n");
        return 0;
    }

    bdf = ext->header.id;
    if ( bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry Dev_Id %#x\n", bdf);
        return 0;
    }

    add_ivrs_mapping_entry(bdf, bdf, ext->header.data_setting,
                           ext->extended_data, false, iommu);

    return dev_length;
}

static u16 __init parse_ivhd_device_extended_range(
    const struct acpi_ivhd_device_extended_range *range,
    u16 header_length, u16 block_length, struct amd_iommu *iommu)
{
    unsigned int dev_length, first_bdf, last_bdf, bdf;

    dev_length = sizeof(*range);
    if ( header_length < (block_length + dev_length) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry length\n");
        return 0;
    }

    if ( range->end.header.type != ACPI_IVRS_TYPE_END )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: End_Type %#x\n",
                        range->end.header.type);
        return 0;
    }

    first_bdf = range->extended.header.id;
    if ( first_bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: First Dev_Id %#x\n", first_bdf);
        return 0;
    }

    last_bdf = range->end.header.id;
    if ( (last_bdf >= ivrs_bdf_entries) || (last_bdf <= first_bdf) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid range: Last Dev_Id %#x\n", last_bdf);
        return 0;
    }

    AMD_IOMMU_DEBUG(" Dev_Id Range: %#x -> %#x\n",
                    first_bdf, last_bdf);

    for ( bdf = first_bdf; bdf <= last_bdf; bdf++ )
        add_ivrs_mapping_entry(bdf, bdf, range->extended.header.data_setting,
                               range->extended.extended_data, false, iommu);

    return dev_length;
}

static int __init cf_check parse_ivrs_ioapic(const char *str)
{
    const char *s = str;
    unsigned long id;
    unsigned int seg, bus, dev, func;
    unsigned int idx;

    if ( *s != '[' )
        return -EINVAL;

    id = simple_strtoul(s + 1, &s, 0);
    if ( *s != ']' || *++s != '=' )
        return -EINVAL;

    s = parse_pci(s + 1, &seg, &bus, &dev, &func);
    if ( !s || *s )
        return -EINVAL;

    idx = ioapic_id_to_index(id);
    if ( idx == MAX_IO_APICS )
    {
        idx = get_next_ioapic_sbdf_index();
        if ( idx == MAX_IO_APICS )
        {
            printk(XENLOG_ERR "Error: %s: Too many IO APICs.\n", __func__);
            return -EINVAL;
        }
    }

    ioapic_sbdf[idx].sbdf = PCI_SBDF(seg, bus, dev, func);
    ioapic_sbdf[idx].id = id;
    ioapic_sbdf[idx].cmdline = true;

    return 0;
}
custom_param("ivrs_ioapic[", parse_ivrs_ioapic);

static int __init cf_check parse_ivrs_hpet(const char *str)
{
    const char *s = str;
    unsigned long id;
    unsigned int seg, bus, dev, func;

    if ( *s != '[' )
        return -EINVAL;

    id = simple_strtoul(s + 1, &s, 0);
    if ( id != (typeof(hpet_sbdf.id))id || *s != ']' || *++s != '=' )
        return -EINVAL;

    s = parse_pci(s + 1, &seg, &bus, &dev, &func);
    if ( !s || *s )
        return -EINVAL;

    hpet_sbdf.id = id;
    hpet_sbdf.sbdf = PCI_SBDF(seg, bus, dev, func);
    hpet_sbdf.init = HPET_CMDL;

    return 0;
}
custom_param("ivrs_hpet[", parse_ivrs_hpet);

static u16 __init parse_ivhd_device_special(
    const struct acpi_ivrs_device8c *special, u16 seg,
    u16 header_length, u16 block_length, struct amd_iommu *iommu)
{
    uint16_t dev_length;
    unsigned int apic, idx;
    pci_sbdf_t sbdf;

    dev_length = sizeof(*special);
    if ( header_length < (block_length + dev_length) )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry length\n");
        return 0;
    }

    sbdf = PCI_SBDF(seg, special->used_id);
    if ( sbdf.bdf >= ivrs_bdf_entries )
    {
        AMD_IOMMU_ERROR("IVHD: invalid Device_Entry Dev_Id %#x\n", sbdf.bdf);
        return 0;
    }

    AMD_IOMMU_DEBUG("IVHD Special: %pp variety %#x handle %#x\n",
                    &sbdf, special->variety, special->handle);
    add_ivrs_mapping_entry(sbdf.bdf, sbdf.bdf, special->header.data_setting, 0, true,
                           iommu);

    switch ( special->variety )
    {
    case ACPI_IVHD_IOAPIC:
        if ( !iommu_intremap )
            break;
        /*
         * Some BIOSes have IOAPIC broken entries so we check for IVRS
         * consistency here --- whether entry's IOAPIC ID is valid and
         * whether there are conflicting/duplicated entries.
         */
        for ( idx = 0; idx < nr_ioapic_sbdf; idx++ )
        {
            if ( ioapic_sbdf[idx].sbdf.sbdf == sbdf.sbdf &&
                 ioapic_sbdf[idx].cmdline )
                break;
        }
        if ( idx < nr_ioapic_sbdf )
        {
            AMD_IOMMU_DEBUG("IVHD: Command line override present for IO-APIC %#x"
                            "(IVRS: %#x devID %pp)\n",
                            ioapic_sbdf[idx].id, special->handle,
                            &sbdf);
            break;
        }

        for ( apic = 0; apic < nr_ioapics; apic++ )
        {
            if ( IO_APIC_ID(apic) != special->handle )
                continue;

            idx = ioapic_id_to_index(special->handle);
            if ( idx != MAX_IO_APICS && ioapic_sbdf[idx].cmdline )
                AMD_IOMMU_DEBUG("IVHD: Command line override present for IO-APIC %#x\n",
                                special->handle);
            else if ( idx != MAX_IO_APICS && ioapic_sbdf[idx].pin_2_idx )
            {
                if ( ioapic_sbdf[idx].sbdf.sbdf == sbdf.sbdf )
                    AMD_IOMMU_WARN("IVHD: duplicate IO-APIC %#x entries\n",
                                    special->handle);
                else
                {
                    AMD_IOMMU_ERROR("IVHD: conflicting IO-APIC %#x entries\n",
                                    special->handle);
                    if ( amd_iommu_perdev_intremap )
                        return 0;
                }
            }
            else
            {
                idx = get_next_ioapic_sbdf_index();
                if ( idx == MAX_IO_APICS )
                {
                    printk(XENLOG_ERR "IVHD Error: Too many IO APICs.\n");
                    return 0;
                }

                /* set device id of ioapic */
                ioapic_sbdf[idx].sbdf = sbdf;
                ioapic_sbdf[idx].id = special->handle;

                ioapic_sbdf[idx].pin_2_idx = xmalloc_array(
                    u16, nr_ioapic_entries[apic]);
                if ( nr_ioapic_entries[apic] &&
                     !ioapic_sbdf[idx].pin_2_idx )
                {
                    printk(XENLOG_ERR "IVHD Error: Out of memory\n");
                    return 0;
                }
                memset(ioapic_sbdf[idx].pin_2_idx, -1,
                       nr_ioapic_entries[apic] *
                       sizeof(*ioapic_sbdf->pin_2_idx));
            }
            break;
        }
        if ( apic == nr_ioapics )
        {
            printk(XENLOG_ERR "IVHD Error: Invalid IO-APIC %#x\n",
                   special->handle);
            return 0;
        }
        break;
    case ACPI_IVHD_HPET:
        switch (hpet_sbdf.init)
        {
        case HPET_IVHD:
            printk(XENLOG_WARNING "Only one IVHD HPET entry is supported.\n");
            break;
        case HPET_CMDL:
            AMD_IOMMU_DEBUG("IVHD: Command line override present for HPET %#x "
                            "(IVRS: %#x devID %pp)\n",
                            hpet_sbdf.id, special->handle,
                            &sbdf);
            break;
        case HPET_NONE:
            /* set device id of hpet */
            hpet_sbdf.id = special->handle;
            hpet_sbdf.sbdf = sbdf;
            hpet_sbdf.init = HPET_IVHD;
            break;
        default:
            ASSERT_UNREACHABLE();
            break;
        }
        break;
    default:
        printk(XENLOG_ERR "Unrecognized IVHD special variety %#x\n",
               special->variety);
        return 0;
    }

    return dev_length;
}

static inline size_t
get_ivhd_header_size(const struct acpi_ivrs_hardware *ivhd_block)
{
    switch ( ivhd_block->header.type )
    {
    case ACPI_IVRS_TYPE_HARDWARE:
        return offsetof(struct acpi_ivrs_hardware, efr_image);
    case ACPI_IVRS_TYPE_HARDWARE_11H:
        return sizeof(struct acpi_ivrs_hardware);
    }
    return 0;
}

static int __init parse_ivhd_block(const struct acpi_ivrs_hardware *ivhd_block)
{
    const union acpi_ivhd_device *ivhd_device;
    u16 block_length, dev_length;
    size_t hdr_size = get_ivhd_header_size(ivhd_block) ;
    struct amd_iommu *iommu;

    if ( ivhd_block->header.length < hdr_size )
    {
        AMD_IOMMU_ERROR("IVHD: invalid block length\n");
        return -ENODEV;
    }

    AMD_IOMMU_DEBUG("IVHD: IOMMU @ %#lx cap @ %#x seg 0x%04x info %#x attr %#x\n",
                    ivhd_block->base_address, ivhd_block->capability_offset,
                    ivhd_block->pci_segment_group, ivhd_block->info,
                    ivhd_block->iommu_attr);

    iommu = find_iommu_from_bdf_cap(ivhd_block->pci_segment_group,
                                    ivhd_block->header.device_id,
                                    ivhd_block->capability_offset);
    if ( !iommu )
    {
        AMD_IOMMU_ERROR("IVHD: no IOMMU for Dev_Id %#x Cap %#x\n",
                        ivhd_block->header.device_id,
                        ivhd_block->capability_offset);
        return -ENODEV;
    }

    /* parse Device Entries */
    block_length = hdr_size;
    while ( ivhd_block->header.length >=
            (block_length + sizeof(struct acpi_ivrs_de_header)) )
    {
        ivhd_device = (const void *)((const u8 *)ivhd_block + block_length);

        AMD_IOMMU_DEBUG("IVHD Device Entry: type %#x id %#x flags %#x\n",
                        ivhd_device->header.type, ivhd_device->header.id,
                        ivhd_device->header.data_setting);

        switch ( ivhd_device->header.type )
        {
        case ACPI_IVRS_TYPE_PAD4:
            dev_length = parse_ivhd_device_padding(
                sizeof(u32),
                ivhd_block->header.length, block_length);
            break;
        case ACPI_IVRS_TYPE_PAD8:
            dev_length = parse_ivhd_device_padding(
                sizeof(u64),
                ivhd_block->header.length, block_length);
            break;
        case ACPI_IVRS_TYPE_SELECT:
            dev_length = parse_ivhd_device_select(&ivhd_device->select, iommu);
            break;
        case ACPI_IVRS_TYPE_START:
            dev_length = parse_ivhd_device_range(
                &ivhd_device->range,
                ivhd_block->header.length, block_length, iommu);
            break;
        case ACPI_IVRS_TYPE_ALIAS_SELECT:
            dev_length = parse_ivhd_device_alias(
                &ivhd_device->alias,
                ivhd_block->header.length, block_length, iommu);
            break;
        case ACPI_IVRS_TYPE_ALIAS_START:
            dev_length = parse_ivhd_device_alias_range(
                &ivhd_device->alias_range,
                ivhd_block->header.length, block_length, iommu);
            break;
        case ACPI_IVRS_TYPE_EXT_SELECT:
            dev_length = parse_ivhd_device_extended(
                &ivhd_device->extended,
                ivhd_block->header.length, block_length, iommu);
            break;
        case ACPI_IVRS_TYPE_EXT_START:
            dev_length = parse_ivhd_device_extended_range(
                &ivhd_device->extended_range,
                ivhd_block->header.length, block_length, iommu);
            break;
        case ACPI_IVRS_TYPE_SPECIAL:
            dev_length = parse_ivhd_device_special(
                &ivhd_device->special, ivhd_block->pci_segment_group,
                ivhd_block->header.length, block_length, iommu);
            break;
        default:
            AMD_IOMMU_WARN("IVHD: unknown device type %#x\n",
                           ivhd_device->header.type);
            dev_length = 0;
            break;
        }

        block_length += dev_length;
        if ( !dev_length )
            return -ENODEV;
    }

    return 0;
}

static void __init dump_acpi_table_header(struct acpi_table_header *table)
{
    int i;

    AMD_IOMMU_DEBUG("ACPI Table:\n");
    AMD_IOMMU_DEBUG(" Signature ");
    for ( i = 0; i < ACPI_NAME_SIZE; i++ )
        printk("%c", table->signature[i]);
    printk("\n");

    AMD_IOMMU_DEBUG(" Length %#x\n", table->length);
    AMD_IOMMU_DEBUG(" Revision %#x\n", table->revision);
    AMD_IOMMU_DEBUG(" CheckSum %#x\n", table->checksum);

    AMD_IOMMU_DEBUG(" OEM_Id ");
    for ( i = 0; i < ACPI_OEM_ID_SIZE; i++ )
        printk("%c", table->oem_id[i]);
    printk("\n");

    AMD_IOMMU_DEBUG(" OEM_Table_Id ");
    for ( i = 0; i < ACPI_OEM_TABLE_ID_SIZE; i++ )
        printk("%c", table->oem_table_id[i]);
    printk("\n");

    AMD_IOMMU_DEBUG(" OEM_Revision %#x\n", table->oem_revision);

    AMD_IOMMU_DEBUG(" Creator_Id ");
    for ( i = 0; i < ACPI_NAME_SIZE; i++ )
        printk("%c", table->asl_compiler_id[i]);
    printk("\n");

    AMD_IOMMU_DEBUG(" Creator_Revision %#x\n",
                    table->asl_compiler_revision);

}

static struct acpi_ivrs_memory __initdata user_ivmds[8];
static unsigned int __initdata nr_ivmd;

#define to_ivhd_block(hdr) \
    container_of(hdr, const struct acpi_ivrs_hardware, header)
#define to_ivmd_block(hdr) \
    container_of(hdr, const struct acpi_ivrs_memory, header)

static inline bool is_ivhd_block(u8 type)
{
    return (type == ACPI_IVRS_TYPE_HARDWARE ||
            ((amd_iommu_acpi_info & ACPI_IVRS_EFR_SUP) &&
             type == ACPI_IVRS_TYPE_HARDWARE_11H));
}

static inline bool is_ivmd_block(u8 type)
{
    return (type == ACPI_IVRS_TYPE_MEMORY_ALL ||
            type == ACPI_IVRS_TYPE_MEMORY_ONE ||
            type == ACPI_IVRS_TYPE_MEMORY_RANGE ||
            type == ACPI_IVRS_TYPE_MEMORY_IOMMU);
}

static int __init cf_check add_one_extra_ivmd(unsigned long start,
                                              unsigned long nr,
                                              uint32_t id, void *ctxt)
{
    struct acpi_ivrs_memory ivmd = {
        .header = {
            .length = sizeof(ivmd),
            .flags = ACPI_IVMD_UNITY | ACPI_IVMD_READ | ACPI_IVMD_WRITE,
            .device_id = id,
            .type = ACPI_IVRS_TYPE_MEMORY_ONE,
        },
    };

    ivmd.start_address = pfn_to_paddr(start);
    ivmd.memory_length = pfn_to_paddr(nr);

    return parse_ivmd_block(&ivmd);
}

static int __init cf_check parse_ivrs_table(struct acpi_table_header *table)
{
    const struct acpi_ivrs_header *ivrs_block;
    unsigned long length;
    unsigned int apic, i;
    bool sb_ioapic = !iommu_intremap;
    int error = 0;

    BUG_ON(!table);

    if ( iommu_debug )
        dump_acpi_table_header(table);

    /* parse IVRS blocks */
    length = sizeof(struct acpi_table_ivrs);
    while ( (error == 0) && (table->length > (length + sizeof(*ivrs_block))) )
    {
        ivrs_block = (struct acpi_ivrs_header *)((u8 *)table + length);

        AMD_IOMMU_DEBUG("IVRS Block: type %#x flags %#x len %#x id %#x\n",
                        ivrs_block->type, ivrs_block->flags,
                        ivrs_block->length, ivrs_block->device_id);

        if ( table->length < (length + ivrs_block->length) )
        {
            AMD_IOMMU_ERROR("IVRS: table length exceeded: %#x -> %#lx\n",
                            table->length,
                            (length + ivrs_block->length));
            return -ENODEV;
        }

        if ( ivrs_block->type == ivhd_type )
            error = parse_ivhd_block(to_ivhd_block(ivrs_block));
        else if ( is_ivmd_block (ivrs_block->type) )
            error = parse_ivmd_block(to_ivmd_block(ivrs_block));
        length += ivrs_block->length;
    }

    /* Add command line specified IVMD-equivalents. */
    if ( nr_ivmd )
        AMD_IOMMU_DEBUG("IVMD: %u command line provided entries\n", nr_ivmd);
    for ( i = 0; !error && i < nr_ivmd; ++i )
        error = parse_ivmd_block(user_ivmds + i);
    if ( !error )
        error = iommu_get_extra_reserved_device_memory(add_one_extra_ivmd, NULL);

    /* Each IO-APIC must have been mentioned in the table. */
    for ( apic = 0; !error && iommu_intremap && apic < nr_ioapics; ++apic )
    {
        unsigned int idx;

        if ( !nr_ioapic_entries[apic] )
            continue;

        idx = ioapic_id_to_index(IO_APIC_ID(apic));
        if ( idx == MAX_IO_APICS )
        {
            printk(XENLOG_ERR "IVHD Error: no information for IO-APIC %#x\n",
                   IO_APIC_ID(apic));
            if ( amd_iommu_perdev_intremap )
                return -ENXIO;
        }

        /* SB IO-APIC is always on this device in AMD systems. */
        if ( ioapic_sbdf[idx].sbdf.sbdf == PCI_SBDF(0, 0, 0x14, 0).sbdf )
            sb_ioapic = 1;

        if ( ioapic_sbdf[idx].pin_2_idx )
            continue;

        ioapic_sbdf[idx].pin_2_idx = xmalloc_array(
            u16, nr_ioapic_entries[apic]);
        if ( ioapic_sbdf[idx].pin_2_idx )
            memset(ioapic_sbdf[idx].pin_2_idx, -1,
                   nr_ioapic_entries[apic] * sizeof(*ioapic_sbdf->pin_2_idx));
        else
        {
            printk(XENLOG_ERR "IVHD Error: Out of memory\n");
            error = -ENOMEM;
        }
    }

    if ( !error && !sb_ioapic )
    {
        if ( amd_iommu_perdev_intremap )
            error = -ENXIO;
        printk("%sNo southbridge IO-APIC found in IVRS table\n",
               amd_iommu_perdev_intremap ? XENLOG_ERR : XENLOG_WARNING);
    }

    return error;
}

static int __init cf_check detect_iommu_acpi(struct acpi_table_header *table)
{
    const struct acpi_ivrs_header *ivrs_block;
    unsigned long length = sizeof(struct acpi_table_ivrs);

    while ( table->length > (length + sizeof(*ivrs_block)) )
    {
        ivrs_block = (struct acpi_ivrs_header *)((u8 *)table + length);
        if ( table->length < (length + ivrs_block->length) )
            return -ENODEV;
        if ( ivrs_block->type == ivhd_type &&
             amd_iommu_detect_one_acpi(to_ivhd_block(ivrs_block)) != 0 )
            return -ENODEV;
        length += ivrs_block->length;
    }
    return 0;
}

#define UPDATE_LAST_BDF(x) do {\
   if ((x) > last_bdf) \
       last_bdf = (x); \
   } while(0);

static int __init get_last_bdf_ivhd(
    const struct acpi_ivrs_hardware *ivhd_block)
{
    const union acpi_ivhd_device *ivhd_device;
    u16 block_length, dev_length;
    size_t hdr_size = get_ivhd_header_size(ivhd_block);
    int last_bdf = 0;

    if ( ivhd_block->header.length < hdr_size )
    {
        AMD_IOMMU_ERROR("IVHD: invalid block length\n");
        return -ENODEV;
    }

    block_length = hdr_size;
    while ( ivhd_block->header.length >=
            (block_length + sizeof(struct acpi_ivrs_de_header)) )
    {
        ivhd_device = (const void *)ivhd_block + block_length;

        switch ( ivhd_device->header.type )
        {
        case ACPI_IVRS_TYPE_PAD4:
            dev_length = sizeof(u32);
            break;
        case ACPI_IVRS_TYPE_PAD8:
            dev_length = sizeof(u64);
            break;
        case ACPI_IVRS_TYPE_SELECT:
            UPDATE_LAST_BDF(ivhd_device->select.header.id);
            dev_length = sizeof(ivhd_device->header);
            break;
        case ACPI_IVRS_TYPE_ALIAS_SELECT:
            UPDATE_LAST_BDF(ivhd_device->alias.header.id);
            dev_length = sizeof(ivhd_device->alias);
            break;
        case ACPI_IVRS_TYPE_EXT_SELECT:
            UPDATE_LAST_BDF(ivhd_device->extended.header.id);
            dev_length = sizeof(ivhd_device->extended);
            break;
        case ACPI_IVRS_TYPE_START:
            UPDATE_LAST_BDF(ivhd_device->range.end.header.id);
            dev_length = sizeof(ivhd_device->range);
            break;
        case ACPI_IVRS_TYPE_ALIAS_START:
            UPDATE_LAST_BDF(ivhd_device->alias_range.end.header.id)
            dev_length = sizeof(ivhd_device->alias_range);
            break;
        case ACPI_IVRS_TYPE_EXT_START:
            UPDATE_LAST_BDF(ivhd_device->extended_range.end.header.id)
            dev_length = sizeof(ivhd_device->extended_range);
            break;
        case ACPI_IVRS_TYPE_SPECIAL:
            UPDATE_LAST_BDF(ivhd_device->special.used_id)
            dev_length = sizeof(ivhd_device->special);
            break;
        default:
            AMD_IOMMU_WARN("IVHD: unknown device type %#x\n",
                           ivhd_device->header.type);
            dev_length = 0;
            break;
        }

        block_length += dev_length;
        if ( !dev_length )
            return -ENODEV;
    }

    return last_bdf;
}

static int __init cf_check cf_check get_last_bdf_acpi(
    struct acpi_table_header *table)
{
    const struct acpi_ivrs_header *ivrs_block;
    unsigned long length = sizeof(struct acpi_table_ivrs);
    int last_bdf = 0;

    while ( table->length > (length + sizeof(*ivrs_block)) )
    {
        ivrs_block = (struct acpi_ivrs_header *)((u8 *)table + length);
        if ( table->length < (length + ivrs_block->length) )
            return -ENODEV;
        if ( ivrs_block->type == ivhd_type )
        {
            int ret = get_last_bdf_ivhd(to_ivhd_block(ivrs_block));

            if ( ret < 0 )
                return ret;
            UPDATE_LAST_BDF(ret);
        }
        length += ivrs_block->length;
    }

    return last_bdf;
}

int __init amd_iommu_detect_acpi(void)
{
    return acpi_table_parse(ACPI_SIG_IVRS, detect_iommu_acpi);
}

int __init amd_iommu_get_ivrs_dev_entries(void)
{
    int ret = acpi_table_parse(ACPI_SIG_IVRS, get_last_bdf_acpi);

    return ret < 0 ? ret : (ret | PCI_FUNC(~0)) + 1;
}

int __init amd_iommu_update_ivrs_mapping_acpi(void)
{
    return acpi_table_parse(ACPI_SIG_IVRS, parse_ivrs_table);
}

static int __init cf_check
get_supported_ivhd_type(struct acpi_table_header *table)
{
    size_t length = sizeof(struct acpi_table_ivrs);
    const struct acpi_ivrs_header *ivrs_block, *blk = NULL;
    uint8_t checksum;

    /* Validate checksum: Sum of entire table == 0. */
    checksum = acpi_tb_checksum(ACPI_CAST_PTR(uint8_t, table), table->length);
    if ( checksum )
    {
        AMD_IOMMU_ERROR("IVRS: invalid checksum %#x\n", checksum);
        return -ENODEV;
    }

    amd_iommu_acpi_info = container_of(table, const struct acpi_table_ivrs,
                                       header)->info;

    while ( table->length > (length + sizeof(*ivrs_block)) )
    {
        ivrs_block = (struct acpi_ivrs_header *)((u8 *)table + length);

        if ( table->length < (length + ivrs_block->length) )
        {
            AMD_IOMMU_ERROR("IVRS: table length exceeded: %#x -> %#lx\n",
                            table->length,
                            (length + ivrs_block->length));
            return -ENODEV;
        }

        if ( is_ivhd_block(ivrs_block->type) &&
            (!blk || blk->type < ivrs_block->type) )
        {
            AMD_IOMMU_DEBUG("IVRS Block: Found type %#x flags %#x len %#x id %#x\n",
                            ivrs_block->type, ivrs_block->flags,
                            ivrs_block->length, ivrs_block->device_id);
            blk = ivrs_block;
        }
        length += ivrs_block->length;
    }

    if ( !blk )
    {
        printk(XENLOG_ERR "Cannot find supported IVHD type.\n");
        return -ENODEV;
    }

    AMD_IOMMU_DEBUG("Using IVHD type %#x\n", blk->type);

    return blk->type;
}

int __init amd_iommu_get_supported_ivhd_type(void)
{
    return acpi_table_parse(ACPI_SIG_IVRS, get_supported_ivhd_type);
}

/*
 * Parse "ivmd" command line option to later add the parsed devices / regions
 * into unity mapping lists, just like IVMDs parsed from ACPI.
 * Format:
 * ivmd=<start>[-<end>][=<bdf1>[-<bdf1>'][,<bdf2>[-<bdf2>'][,...]]][;<start>...]
 */
static int __init cf_check parse_ivmd_param(const char *s)
{
    do {
        unsigned long start, end;
        const char *cur;

        if ( nr_ivmd >= ARRAY_SIZE(user_ivmds) )
            return -E2BIG;

        start = simple_strtoul(cur = s, &s, 16);
        if ( cur == s )
            return -EINVAL;

        if ( *s == '-' )
        {
            end = simple_strtoul(cur = s + 1, &s, 16);
            if ( cur == s || end < start )
                return -EINVAL;
        }
        else
            end = start;

        if ( *s != '=' )
        {
            user_ivmds[nr_ivmd].start_address = start << PAGE_SHIFT;
            user_ivmds[nr_ivmd].memory_length = (end - start + 1) << PAGE_SHIFT;
            user_ivmds[nr_ivmd].header.flags = ACPI_IVMD_UNITY |
                                               ACPI_IVMD_READ | ACPI_IVMD_WRITE;
            user_ivmds[nr_ivmd].header.length = sizeof(*user_ivmds);
            user_ivmds[nr_ivmd].header.type = ACPI_IVRS_TYPE_MEMORY_ALL;
            ++nr_ivmd;
            continue;
        }

        do {
            unsigned int seg, bus, dev, func;

            if ( nr_ivmd >= ARRAY_SIZE(user_ivmds) )
                return -E2BIG;

            s = parse_pci(s + 1, &seg, &bus, &dev, &func);
            if ( !s || seg )
                return -EINVAL;

            user_ivmds[nr_ivmd].start_address = start << PAGE_SHIFT;
            user_ivmds[nr_ivmd].memory_length = (end - start + 1) << PAGE_SHIFT;
            user_ivmds[nr_ivmd].header.flags = ACPI_IVMD_UNITY |
                                               ACPI_IVMD_READ | ACPI_IVMD_WRITE;
            user_ivmds[nr_ivmd].header.length = sizeof(*user_ivmds);
            user_ivmds[nr_ivmd].header.device_id = PCI_BDF(bus, dev, func);
            user_ivmds[nr_ivmd].header.type = ACPI_IVRS_TYPE_MEMORY_ONE;

            if ( *s == '-' )
            {
                s = parse_pci(s + 1, &seg, &bus, &dev, &func);
                if ( !s || seg )
                    return -EINVAL;

                user_ivmds[nr_ivmd].aux_data = PCI_BDF(bus, dev, func);
                if ( user_ivmds[nr_ivmd].aux_data <
                     user_ivmds[nr_ivmd].header.device_id )
                    return -EINVAL;
                user_ivmds[nr_ivmd].header.type = ACPI_IVRS_TYPE_MEMORY_RANGE;
            }
        } while ( ++nr_ivmd, *s == ',' );
    } while ( *s++ == ';' );

    return s[-1] ? -EINVAL : 0;
}
custom_param("ivmd", parse_ivmd_param);
