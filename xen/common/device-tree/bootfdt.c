/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Early Device Tree
 *
 * Copyright (C) 2012-2014 Citrix Systems, Inc.
 */

#include <xen/bootfdt.h>
#include <xen/device_tree.h>
#include <xen/efi.h>
#include <xen/init.h>
#include <xen/kernel.h>
#include <xen/lib.h>
#include <xen/libfdt/libfdt-xen.h>
#include <xen/sort.h>
#include <xsm/xsm.h>
#include <asm/setup.h>
#ifdef CONFIG_STATIC_SHM
#include <asm/static-shmem.h>
#endif

static void __init __maybe_unused build_assertions(void)
{
    /*
     * Check that no padding is between struct membanks "bank" flexible array
     * member and struct meminfo "bank" member
     */
    BUILD_BUG_ON((offsetof(struct membanks, bank) !=
                 offsetof(struct meminfo, bank)));
    /* Ensure "struct membanks" is 8-byte aligned */
    BUILD_BUG_ON(alignof(struct membanks) != 8);
}

static bool __init device_tree_node_is_available(const void *fdt, int node)
{
    const char *status;
    int len;

    status = fdt_getprop(fdt, node, "status", &len);
    if ( !status )
        return true;

    if ( len > 0 )
    {
        if ( !strcmp(status, "ok") || !strcmp(status, "okay") )
            return true;
    }

    return false;
}

static bool __init device_tree_node_matches(const void *fdt, int node,
                                            const char *match)
{
    const char *name;
    size_t match_len;

    name = fdt_get_name(fdt, node, NULL);
    match_len = strlen(match);

    /* Match both "match" and "match@..." patterns but not
       "match-foo". */
    return strncmp(name, match, match_len) == 0
        && (name[match_len] == '@' || name[match_len] == '\0');
}

static bool __init device_tree_node_compatible(const void *fdt, int node,
                                               const char *match)
{
    int len, l;
    const void *prop;

    prop = fdt_getprop(fdt, node, "compatible", &len);
    if ( prop == NULL )
        return false;

    while ( len > 0 ) {
        if ( !dt_compat_cmp(prop, match) )
            return true;
        l = strlen(prop) + 1;
        prop += l;
        len -= l;
    }

    return false;
}

/*
 * Check if a node is a proper /memory node according to Devicetree
 * Specification v0.4, chapter 3.4.
 */
static bool __init device_tree_is_memory_node(const void *fdt, int node,
                                              int depth)
{
    const char *type;
    int len;

    if ( depth != 1 )
        return false;

    if ( !device_tree_node_matches(fdt, node, "memory") )
        return false;

    type = fdt_getprop(fdt, node, "device_type", &len);
    if ( !type )
        return false;

    if ( (len <= strlen("memory")) || strcmp(type, "memory") )
        return false;

    return true;
}

void __init device_tree_get_reg(const __be32 **cell, uint32_t address_cells,
                                uint32_t size_cells, paddr_t *start,
                                paddr_t *size)
{
    uint64_t dt_start, dt_size;

    /*
     * dt_next_cell will return uint64_t whereas paddr_t may not be 64-bit.
     * Thus, there is an implicit cast from uint64_t to paddr_t.
     */
    dt_start = dt_next_cell(address_cells, cell);
    dt_size = dt_next_cell(size_cells, cell);

    if ( dt_start != (paddr_t)dt_start )
    {
        printk("Physical address greater than max width supported\n");
        WARN();
    }

    if ( dt_size != (paddr_t)dt_size )
    {
        printk("Physical size greater than max width supported\n");
        WARN();
    }

    /*
     * Xen will truncate the address/size if it is greater than the maximum
     * supported width and it will give an appropriate warning.
     */
    *start = dt_start;
    *size = dt_size;
}

static int __init device_tree_get_meminfo(const void *fdt, int node,
                                          const char *prop_name,
                                          u32 address_cells, u32 size_cells,
                                          struct membanks *mem,
                                          enum membank_type type)
{
    const struct fdt_property *prop;
    unsigned int i, banks;
    const __be32 *cell;
    u32 reg_cells = address_cells + size_cells;
    paddr_t start, size;

    if ( !device_tree_node_is_available(fdt, node) )
        return 0;

    if ( address_cells < 1 || size_cells < 1 )
    {
        printk("fdt: property `%s': invalid #address-cells or #size-cells",
               prop_name);
        return -EINVAL;
    }

    prop = fdt_get_property(fdt, node, prop_name, NULL);
    if ( !prop )
        return -ENOENT;

    cell = (const __be32 *)prop->data;
    banks = fdt32_to_cpu(prop->len) / (reg_cells * sizeof (u32));

    for ( i = 0; i < banks && mem->nr_banks < mem->max_banks; i++ )
    {
        device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
        if ( mem == bootinfo_get_reserved_mem() &&
             check_reserved_regions_overlap(start, size) )
            return -EINVAL;
        /* Some DT may describe empty bank, ignore them */
        if ( !size )
            continue;
        mem->bank[mem->nr_banks].start = start;
        mem->bank[mem->nr_banks].size = size;
        mem->bank[mem->nr_banks].type = type;
        mem->nr_banks++;
    }

    if ( i < banks )
    {
        printk("Warning: Max number of supported memory regions reached.\n");
        return -ENOSPC;
    }

    return 0;
}

u32 __init device_tree_get_u32(const void *fdt, int node,
                               const char *prop_name, u32 dflt)
{
    const struct fdt_property *prop;

    prop = fdt_get_property(fdt, node, prop_name, NULL);
    if ( !prop || prop->len < sizeof(u32) )
        return dflt;

    return fdt32_to_cpu(*(uint32_t*)prop->data);
}

/**
 * device_tree_for_each_node - iterate over all device tree sub-nodes
 * @fdt: flat device tree.
 * @node: parent node to start the search from
 * @func: function to call for each sub-node.
 * @data: data to pass to @func.
 *
 * Any nodes nested at DEVICE_TREE_MAX_DEPTH or deeper are ignored.
 *
 * Returns 0 if all nodes were iterated over successfully.  If @func
 * returns a value different from 0, that value is returned immediately.
 */
int __init device_tree_for_each_node(const void *fdt, int node,
                                     device_tree_node_func func,
                                     void *data)
{
    /*
     * We only care about relative depth increments, assume depth of
     * node is 0 for simplicity.
     */
    int depth = 0;
    const int first_node = node;
    u32 address_cells[DEVICE_TREE_MAX_DEPTH];
    u32 size_cells[DEVICE_TREE_MAX_DEPTH];
    int ret;

    do {
        const char *name = fdt_get_name(fdt, node, NULL);
        u32 as, ss;

        if ( depth >= DEVICE_TREE_MAX_DEPTH )
        {
            printk("Warning: device tree node `%s' is nested too deep\n",
                   name);
            continue;
        }

        as = depth > 0 ? address_cells[depth-1] : DT_ROOT_NODE_ADDR_CELLS_DEFAULT;
        ss = depth > 0 ? size_cells[depth-1] : DT_ROOT_NODE_SIZE_CELLS_DEFAULT;

        address_cells[depth] = device_tree_get_u32(fdt, node,
                                                   "#address-cells", as);
        size_cells[depth] = device_tree_get_u32(fdt, node,
                                                "#size-cells", ss);

        /* skip the first node */
        if ( node != first_node )
        {
            ret = func(fdt, node, name, depth, as, ss, data);
            if ( ret != 0 )
                return ret;
        }

        node = fdt_next_node(fdt, node, &depth);
    } while ( node >= 0 && depth > 0 );

    return 0;
}

static int __init process_memory_node(const void *fdt, int node,
                                      const char *name, int depth,
                                      u32 address_cells, u32 size_cells,
                                      struct membanks *mem)
{
    return device_tree_get_meminfo(fdt, node, "reg", address_cells, size_cells,
                                   mem, MEMBANK_DEFAULT);
}

static int __init process_reserved_memory_node(const void *fdt, int node,
                                               const char *name, int depth,
                                               u32 address_cells,
                                               u32 size_cells,
                                               void *data)
{
    int rc = process_memory_node(fdt, node, name, depth, address_cells,
                                 size_cells, data);

    if ( rc == -ENOSPC )
        panic("Max number of supported reserved-memory regions reached.\n");
    else if ( rc != -ENOENT )
        return rc;
    return 0;
}

static int __init process_reserved_memory(const void *fdt, int node,
                                          const char *name, int depth,
                                          u32 address_cells, u32 size_cells)
{
    return device_tree_for_each_node(fdt, node,
                                     process_reserved_memory_node,
                                     bootinfo_get_reserved_mem());
}

static void __init process_multiboot_node(const void *fdt, int node,
                                          const char *name,
                                          u32 address_cells, u32 size_cells)
{
    static int __initdata kind_guess = 0;
    const struct fdt_property *prop;
    const __be32 *cell;
    bootmodule_kind kind;
    paddr_t start, size;
    int len;
    /* sizeof("/chosen/") + DT_MAX_NAME + '/' + DT_MAX_NAME + '/0' => 92 */
    char path[92];
    int parent_node, ret;
    bool domU;

    parent_node = fdt_parent_offset(fdt, node);
    ASSERT(parent_node >= 0);

    /* Check that the node is under "/chosen" (first 7 chars of path) */
    ret = fdt_get_path(fdt, node, path, sizeof (path));
    if ( ret != 0 || strncmp(path, "/chosen", 7) )
        return;

    prop = fdt_get_property(fdt, node, "reg", &len);
    if ( !prop )
        panic("node %s missing `reg' property\n", name);

    if ( len < dt_cells_to_size(address_cells + size_cells) )
        panic("fdt: node `%s': `reg` property length is too short\n",
                    name);

    cell = (const __be32 *)prop->data;
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);

    if ( fdt_node_check_compatible(fdt, node, "xen,linux-zimage") == 0 ||
         fdt_node_check_compatible(fdt, node, "multiboot,kernel") == 0 )
        kind = BOOTMOD_KERNEL;
    else if ( fdt_node_check_compatible(fdt, node, "xen,linux-initrd") == 0 ||
              fdt_node_check_compatible(fdt, node, "multiboot,ramdisk") == 0 )
        kind = BOOTMOD_RAMDISK;
    else if ( fdt_node_check_compatible(fdt, node, "xen,xsm-policy") == 0 )
        kind = BOOTMOD_XSM;
    else if ( fdt_node_check_compatible(fdt, node, "multiboot,device-tree") == 0 )
        kind = BOOTMOD_GUEST_DTB;
    else
        kind = BOOTMOD_UNKNOWN;

    /**
     * Guess the kind of these first two unknowns respectively:
     * (1) The first unknown must be kernel.
     * (2) Detect the XSM Magic from the 2nd unknown:
     *     a. If it's XSM, set the kind as XSM, and that also means we
     *     won't load ramdisk;
     *     b. if it's not XSM, set the kind as ramdisk.
     *     So if user want to load ramdisk, it must be the 2nd unknown.
     * We also detect the XSM Magic for the following unknowns,
     * then set its kind according to the return value of has_xsm_magic.
     */
    if ( kind == BOOTMOD_UNKNOWN )
    {
        switch ( kind_guess++ )
        {
        case 0: kind = BOOTMOD_KERNEL; break;
        case 1: kind = BOOTMOD_RAMDISK; break;
        default: break;
        }
        if ( kind_guess > 1 && has_xsm_magic(start) )
            kind = BOOTMOD_XSM;
    }

    domU = fdt_node_check_compatible(fdt, parent_node, "xen,domain") == 0;
    add_boot_module(kind, start, size, domU);

    prop = fdt_get_property(fdt, node, "bootargs", &len);
    if ( !prop )
        return;
    add_boot_cmdline(fdt_get_name(fdt, parent_node, &len), prop->data,
                     kind, start, domU);
}

static int __init process_chosen_node(const void *fdt, int node,
                                      const char *name,
                                      u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    paddr_t start, end;
    int len;

    if ( fdt_get_property(fdt, node, "xen,static-heap", NULL) )
    {
        int rc;

        printk("Checking for static heap in /chosen\n");

        rc = device_tree_get_meminfo(fdt, node, "xen,static-heap",
                                     address_cells, size_cells,
                                     bootinfo_get_reserved_mem(),
                                     MEMBANK_STATIC_HEAP);
        if ( rc )
            return rc;

        bootinfo.static_heap = true;
    }

    printk("Checking for initrd in /chosen\n");

    prop = fdt_get_property(fdt, node, "linux,initrd-start", &len);
    if ( !prop )
        /* No initrd present. */
        return 0;
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,initrd-start property has invalid length %d\n", len);
        return -EINVAL;
    }
    start = dt_read_paddr((const void *)&prop->data, dt_size_to_cells(len));

    prop = fdt_get_property(fdt, node, "linux,initrd-end", &len);
    if ( !prop )
    {
        printk("linux,initrd-end not present but -start was\n");
        return -EINVAL;
    }
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,initrd-end property has invalid length %d\n", len);
        return -EINVAL;
    }
    end = dt_read_paddr((const void *)&prop->data, dt_size_to_cells(len));

    if ( start >= end )
    {
        printk("linux,initrd limits invalid: %"PRIpaddr" >= %"PRIpaddr"\n",
                  start, end);
        return -EINVAL;
    }

    printk("Initrd %"PRIpaddr"-%"PRIpaddr"\n", start, end);

    add_boot_module(BOOTMOD_RAMDISK, start, end-start, false);

    return 0;
}

static int __init process_domain_node(const void *fdt, int node,
                                      const char *name,
                                      u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;

    printk("Checking for \"xen,static-mem\" in domain node\n");

    prop = fdt_get_property(fdt, node, "xen,static-mem", NULL);
    if ( !prop )
        /* No "xen,static-mem" present. */
        return 0;

    return device_tree_get_meminfo(fdt, node, "xen,static-mem", address_cells,
                                   size_cells, bootinfo_get_reserved_mem(),
                                   MEMBANK_STATIC_DOMAIN);
}

#ifndef CONFIG_STATIC_SHM
static inline int process_shm_node(const void *fdt, int node,
                                   uint32_t address_cells, uint32_t size_cells)
{
    printk("CONFIG_STATIC_SHM must be enabled for parsing static shared"
            " memory nodes\n");
    return -EINVAL;
}
#endif

static int __init early_scan_node(const void *fdt,
                                  int node, const char *name, int depth,
                                  u32 address_cells, u32 size_cells,
                                  void *data)
{
    int rc = 0;

    /*
     * If Xen has been booted via UEFI, the memory banks are
     * populated. So we should skip the parsing.
     */
    if ( !efi_enabled(EFI_BOOT) &&
         device_tree_is_memory_node(fdt, node, depth) )
        rc = process_memory_node(fdt, node, name, depth,
                                 address_cells, size_cells, bootinfo_get_mem());
    else if ( depth == 1 && !dt_node_cmp(name, "reserved-memory") )
        rc = process_reserved_memory(fdt, node, name, depth,
                                     address_cells, size_cells);
    else if ( depth <= 3 && (device_tree_node_compatible(fdt, node, "xen,multiboot-module" ) ||
              device_tree_node_compatible(fdt, node, "multiboot,module" )))
        process_multiboot_node(fdt, node, name, address_cells, size_cells);
    else if ( depth == 1 && device_tree_node_matches(fdt, node, "chosen") )
        rc = process_chosen_node(fdt, node, name, address_cells, size_cells);
    else if ( depth == 2 && device_tree_node_compatible(fdt, node, "xen,domain") )
        rc = process_domain_node(fdt, node, name, address_cells, size_cells);
    else if ( depth <= 3 && device_tree_node_compatible(fdt, node, "xen,domain-shared-memory-v1") )
        rc = process_shm_node(fdt, node, address_cells, size_cells);

    if ( rc < 0 )
        printk("fdt: node `%s': parsing failed\n", name);
    return rc;
}

static void __init early_print_info(void)
{
    const struct membanks *mi = bootinfo_get_mem();
    const struct membanks *mem_resv = bootinfo_get_reserved_mem();
    struct bootmodules *mods = &bootinfo.modules;
    struct bootcmdlines *cmds = &bootinfo.cmdlines;
    unsigned int i;

    for ( i = 0; i < mi->nr_banks; i++ )
        printk("RAM: %"PRIpaddr" - %"PRIpaddr"\n",
                mi->bank[i].start,
                mi->bank[i].start + mi->bank[i].size - 1);
    printk("\n");
    for ( i = 0 ; i < mods->nr_mods; i++ )
        printk("MODULE[%d]: %"PRIpaddr" - %"PRIpaddr" %-12s\n",
                i,
                mods->module[i].start,
                mods->module[i].start + mods->module[i].size,
                boot_module_kind_as_string(mods->module[i].kind));

    for ( i = 0; i < mem_resv->nr_banks; i++ )
    {
        printk(" RESVD[%u]: %"PRIpaddr" - %"PRIpaddr"\n", i,
               mem_resv->bank[i].start,
               mem_resv->bank[i].start + mem_resv->bank[i].size - 1);
    }
#ifdef CONFIG_STATIC_SHM
    early_print_info_shmem();
#endif
    printk("\n");
    for ( i = 0 ; i < cmds->nr_mods; i++ )
        printk("CMDLINE[%"PRIpaddr"]:%s %s\n", cmds->cmdline[i].start,
               cmds->cmdline[i].dt_name,
               &cmds->cmdline[i].cmdline[0]);
    printk("\n");
}

/* This function assumes that memory regions are not overlapped */
static int __init cmp_memory_node(const void *key, const void *elem)
{
    const struct membank *handler0 = key;
    const struct membank *handler1 = elem;

    if ( handler0->start < handler1->start )
        return -1;

    if ( handler0->start >= (handler1->start + handler1->size) )
        return 1;

    return 0;
}

static void __init swap_memory_node(void *_a, void *_b, size_t size)
{
    struct membank *a = _a, *b = _b;

    SWAP(*a, *b);
}

/**
 * boot_fdt_info - initialize bootinfo from a DTB
 * @fdt: flattened device tree binary
 *
 * Returns the size of the DTB.
 */
size_t __init boot_fdt_info(const void *fdt, paddr_t paddr)
{
    struct membanks *reserved_mem = bootinfo_get_reserved_mem();
    struct membanks *mem = bootinfo_get_mem();
    unsigned int i;
    int nr_rsvd;
    int ret;

    ret = fdt_check_header(fdt);
    if ( ret < 0 )
        panic("No valid device tree\n");

    add_boot_module(BOOTMOD_FDT, paddr, fdt_totalsize(fdt), false);

    nr_rsvd = fdt_num_mem_rsv(fdt);
    if ( nr_rsvd < 0 )
        panic("Parsing FDT memory reserve map failed (%d)\n", nr_rsvd);

    for ( i = 0; i < nr_rsvd; i++ )
    {
        struct membank *bank;
        paddr_t s, sz;

        if ( fdt_get_mem_rsv_paddr(device_tree_flattened, i, &s, &sz) < 0 )
            continue;

        continue; //HACK
        if ( reserved_mem->nr_banks < reserved_mem->max_banks )
        {
            bank = &reserved_mem->bank[reserved_mem->nr_banks];
            bank->start = s;
            bank->size = sz;
            bank->type = MEMBANK_FDT_RESVMEM;
            reserved_mem->nr_banks++;
        }
        else
            panic("Cannot allocate reserved memory bank\n");
    }

    ret = device_tree_for_each_node(fdt, 0, early_scan_node, NULL);
    if ( ret )
        panic("Early FDT parsing failed (%d)\n", ret);

    /*
     * On Arm64 setup_directmap_mappings() expects to be called with the lowest
     * bank in memory first. There is no requirement that the DT will provide
     * the banks sorted in ascending order. So sort them through.
     */
    sort(mem->bank, mem->nr_banks, sizeof(struct membank),
         cmp_memory_node, swap_memory_node);

    early_print_info();

    return fdt_totalsize(fdt);
}

const __init char *boot_fdt_cmdline(const void *fdt)
{
    int node;
    const struct fdt_property *prop;

    node = fdt_path_offset(fdt, "/chosen");
    if ( node < 0 )
        return NULL;

    prop = fdt_get_property(fdt, node, "xen,xen-bootargs", NULL);
    if ( prop == NULL )
    {
        struct bootcmdline *dom0_cmdline =
            boot_cmdline_find_by_kind(BOOTMOD_KERNEL);

        if (fdt_get_property(fdt, node, "xen,dom0-bootargs", NULL) ||
            ( dom0_cmdline && dom0_cmdline->cmdline[0] ) )
            prop = fdt_get_property(fdt, node, "bootargs", NULL);
    }
    if ( prop == NULL )
        return NULL;

    return prop->data;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
