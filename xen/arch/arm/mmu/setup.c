/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xen/arch/arm/mmu/setup.c
 *
 * MMU system boot CPU MM bringup code.
 */

#include <xen/init.h>
#include <xen/libfdt/libfdt.h>
#include <xen/llc-coloring.h>
#include <xen/sections.h>
#include <xen/sizes.h>
#include <xen/vmap.h>

#include <asm/setup.h>
#include <asm/fixmap.h>

/* Override macros from asm/page.h to make them work with mfn_t */
#undef mfn_to_virt
#define mfn_to_virt(mfn) __mfn_to_virt(mfn_x(mfn))
#undef virt_to_mfn
#define virt_to_mfn(va) _mfn(__virt_to_mfn(va))

#define virt_to_reloc_virt(virt) \
    (((vaddr_t)(virt)) - XEN_VIRT_START + BOOT_RELOC_VIRT_START)

/* Main runtime page tables */

/*
 * For arm32 xen_pgtable are per-PCPU and are allocated before
 * bringing up each CPU. For arm64 xen_pgtable is common to all PCPUs.
 *
 * xen_second, xen_fixmap and xen_xenmap are always shared between all
 * PCPUs.
 */

#ifdef CONFIG_ARM_64
DEFINE_PAGE_TABLE(xen_pgtable);
static DEFINE_PAGE_TABLE(xen_first);
#define THIS_CPU_PGTABLE xen_pgtable
#else
/* Per-CPU pagetable pages */
/* xen_pgtable == root of the trie (zeroeth level on 64-bit, first on 32-bit) */
DEFINE_PER_CPU(lpae_t *, xen_pgtable);
#define THIS_CPU_PGTABLE this_cpu(xen_pgtable)
/* Root of the trie for cpu0, other CPU's PTs are dynamically allocated */
static DEFINE_PAGE_TABLE(cpu0_pgtable);
#endif

/* Common pagetable leaves */
/* Second level page table used to cover Xen virtual address space */
static DEFINE_PAGE_TABLE(xen_second);
/* Third level page table used for fixmap */
DEFINE_BOOT_PAGE_TABLE(xen_fixmap);
/*
 * Third level page table used to map Xen itself with the XN bit set
 * as appropriate.
 */
static DEFINE_PAGE_TABLES(xen_xenmap, XEN_NR_ENTRIES(2));

/* Limits of the Xen heap */
mfn_t directmap_mfn_start __read_mostly = INVALID_MFN_INITIALIZER;
mfn_t directmap_mfn_end __read_mostly;
vaddr_t directmap_virt_end __read_mostly;
#ifdef CONFIG_ARM_64
vaddr_t directmap_virt_start __read_mostly;
unsigned long directmap_base_pdx __read_mostly;
#endif

/* Checking VA memory layout alignment. */
static void __init __maybe_unused build_assertions(void)
{
    /* 2MB aligned regions */
    BUILD_BUG_ON(XEN_VIRT_START & ~SECOND_MASK);
    BUILD_BUG_ON(FIXMAP_ADDR(0) & ~SECOND_MASK);
    BUILD_BUG_ON(BOOT_RELOC_VIRT_START & ~SECOND_MASK);
    /* 1GB aligned regions */
#ifdef CONFIG_ARM_32
    BUILD_BUG_ON(XENHEAP_VIRT_START & ~FIRST_MASK);
#else
    BUILD_BUG_ON(DIRECTMAP_VIRT_START & ~FIRST_MASK);
#endif
    /* Page table structure constraints */
#ifdef CONFIG_ARM_64
    /*
     * The first few slots of the L0 table is reserved for the identity
     * mapping. Check that none of the other regions are overlapping
     * with it.
     */
#define CHECK_OVERLAP_WITH_IDMAP(virt) \
    BUILD_BUG_ON(zeroeth_table_offset(virt) < IDENTITY_MAPPING_AREA_NR_L0)

    CHECK_OVERLAP_WITH_IDMAP(XEN_VIRT_START);
    CHECK_OVERLAP_WITH_IDMAP(VMAP_VIRT_START);
    CHECK_OVERLAP_WITH_IDMAP(FRAMETABLE_VIRT_START);
    CHECK_OVERLAP_WITH_IDMAP(DIRECTMAP_VIRT_START);
#undef CHECK_OVERLAP_WITH_IDMAP
#endif
    BUILD_BUG_ON(first_table_offset(XEN_VIRT_START));
#ifdef CONFIG_ARCH_MAP_DOMAIN_PAGE
    BUILD_BUG_ON(DOMHEAP_VIRT_START & ~FIRST_MASK);
#endif
    /*
     * The boot code expects the regions XEN_VIRT_START, FIXMAP_ADDR(0),
     * BOOT_FDT_VIRT_START to use the same 0th (arm64 only) and 1st
     * slot in the page tables.
     */
#define CHECK_SAME_SLOT(level, virt1, virt2) \
    BUILD_BUG_ON(level##_table_offset(virt1) != level##_table_offset(virt2))

#define CHECK_DIFFERENT_SLOT(level, virt1, virt2) \
    BUILD_BUG_ON(level##_table_offset(virt1) == level##_table_offset(virt2))

#ifdef CONFIG_ARM_64
    CHECK_SAME_SLOT(zeroeth, XEN_VIRT_START, FIXMAP_ADDR(0));
    CHECK_SAME_SLOT(zeroeth, XEN_VIRT_START, BOOT_FDT_VIRT_START);
#endif
    CHECK_SAME_SLOT(first, XEN_VIRT_START, FIXMAP_ADDR(0));
    CHECK_SAME_SLOT(first, XEN_VIRT_START, BOOT_FDT_VIRT_START);

    /*
     * For arm32, the temporary mapping will re-use the domheap
     * first slot and the second slots will match.
     */
#ifdef CONFIG_ARM_32
    CHECK_SAME_SLOT(first, TEMPORARY_XEN_VIRT_START, DOMHEAP_VIRT_START);
    CHECK_DIFFERENT_SLOT(first, XEN_VIRT_START, TEMPORARY_XEN_VIRT_START);
    CHECK_SAME_SLOT(first, TEMPORARY_XEN_VIRT_START,
                    TEMPORARY_FIXMAP_VIRT_START);
    CHECK_SAME_SLOT(second, XEN_VIRT_START, TEMPORARY_XEN_VIRT_START);
    CHECK_SAME_SLOT(second, FIXMAP_VIRT_START, TEMPORARY_FIXMAP_VIRT_START);
#endif

#undef CHECK_SAME_SLOT
#undef CHECK_DIFFERENT_SLOT

    /*
     * Fixmaps must not overlap with boot FDT mapping area. Make sure there's
     * at least one guard page in between.
     */
    BUILD_BUG_ON(FIXADDR_TOP >= BOOT_FDT_VIRT_START);
}

lpae_t __init pte_of_xenaddr(vaddr_t va)
{
    if ( llc_coloring_enabled )
        va = virt_to_reloc_virt(va);

    return mfn_to_xen_entry(virt_to_mfn(va), MT_NORMAL);
}

void * __init early_fdt_map(paddr_t fdt_paddr)
{
    /* We are using 2MB superpage for mapping the FDT */
    paddr_t base_paddr = fdt_paddr & SECOND_MASK;
    paddr_t offset;
    void *fdt_virt;
    uint32_t size;
    int rc;

    /*
     * Check whether the physical FDT address is set and meets the minimum
     * alignment requirement. Since we are relying on MIN_FDT_ALIGN to be at
     * least 8 bytes so that we always access the magic and size fields
     * of the FDT header after mapping the first chunk, double check if
     * that is indeed the case.
     */
    BUILD_BUG_ON(MIN_FDT_ALIGN < 8);
    if ( !fdt_paddr || fdt_paddr % MIN_FDT_ALIGN )
        return NULL;

    /* The FDT is mapped using 2MB superpage */
    BUILD_BUG_ON(BOOT_FDT_VIRT_START % SZ_2M);

    rc = map_pages_to_xen(BOOT_FDT_VIRT_START, maddr_to_mfn(base_paddr),
                          SZ_2M >> PAGE_SHIFT,
                          PAGE_HYPERVISOR_RO | _PAGE_BLOCK);
    if ( rc )
        panic("Unable to map the device-tree.\n");


    offset = fdt_paddr % SECOND_SIZE;
    fdt_virt = (void *)BOOT_FDT_VIRT_START + offset;

    if ( fdt_magic(fdt_virt) != FDT_MAGIC )
        return NULL;

    size = fdt_totalsize(fdt_virt);
    if ( size > MAX_FDT_SIZE )
        return NULL;

    if ( (offset + size) > SZ_2M )
    {
        rc = map_pages_to_xen(BOOT_FDT_VIRT_START + SZ_2M,
                              maddr_to_mfn(base_paddr + SZ_2M),
                              SZ_2M >> PAGE_SHIFT,
                              PAGE_HYPERVISOR_RO | _PAGE_BLOCK);
        if ( rc )
            panic("Unable to map the device-tree\n");
    }

    return fdt_virt;
}

void __init remove_early_mappings(void)
{
    int rc;

    /* destroy the _PAGE_BLOCK mapping */
    rc = modify_xen_mappings(BOOT_FDT_VIRT_START,
                             BOOT_FDT_VIRT_START + BOOT_FDT_VIRT_SIZE,
                             _PAGE_BLOCK);
    BUG_ON(rc);
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

/*
 * Returns the end address of the highest region in the range s..e
 * with required size and alignment that does not conflict with the
 * modules from first_mod to nr_modules.
 *
 * For non-recursive callers first_mod should normally be 0 (all
 * modules and Xen itself) or 1 (all modules but not Xen).
 */
paddr_t __init consider_modules(paddr_t s, paddr_t e,
                                uint32_t size, paddr_t align,
                                int first_mod)
{
    const struct membanks *reserved_mem = bootinfo_get_reserved_mem();
#ifdef CONFIG_STATIC_SHM
    const struct membanks *shmem = bootinfo_get_shmem();
#endif
    const struct bootmodules *mi = &bootinfo.modules;
    int i;
    int nr;

    s = (s+align-1) & ~(align-1);
    e = e & ~(align-1);

    if ( s > e ||  e - s < size )
        return 0;

    /* First check the boot modules */
    for ( i = first_mod; i < mi->nr_mods; i++ )
    {
        paddr_t mod_s = mi->module[i].start;
        paddr_t mod_e = mod_s + mi->module[i].size;

        if ( s < mod_e && mod_s < e )
        {
            mod_e = consider_modules(mod_e, e, size, align, i+1);
            if ( mod_e )
                return mod_e;

            return consider_modules(s, mod_s, size, align, i+1);
        }
    }

    /*
     * i is the current bootmodule we are evaluating, across all
     * possible kinds of bootmodules.
     *
     * When retrieving the corresponding reserved-memory addresses, we
     * need to index the reserved_mem bank starting from 0, and only counting
     * the reserved-memory modules. Hence, we need to use i - nr.
     */
    nr = mi->nr_mods;
    for ( ; i - nr < reserved_mem->nr_banks; i++ )
    {
        paddr_t r_s = reserved_mem->bank[i - nr].start;
        paddr_t r_e = r_s + reserved_mem->bank[i - nr].size;

        if ( s < r_e && r_s < e )
        {
            r_e = consider_modules(r_e, e, size, align, i + 1);
            if ( r_e )
                return r_e;

            return consider_modules(s, r_s, size, align, i + 1);
        }
    }

#ifdef CONFIG_STATIC_SHM
    nr += reserved_mem->nr_banks;
    for ( ; i - nr < shmem->nr_banks; i++ )
    {
        paddr_t r_s, r_e;

        r_s = shmem->bank[i - nr].start;

        /* Shared memory banks can contain INVALID_PADDR as start */
        if ( INVALID_PADDR == r_s )
            continue;

        r_e = r_s + shmem->bank[i - nr].size;

        if ( s < r_e && r_s < e )
        {
            r_e = consider_modules(r_e, e, size, align, i + 1);
            if ( r_e )
                return r_e;

            return consider_modules(s, r_s, size, align, i + 1);
        }
    }
#endif

    return e;
}

static void __init create_llc_coloring_mappings(void)
{
    lpae_t pte;
    unsigned int i;
    struct bootmodule *xen_bootmodule = boot_module_find_by_kind(BOOTMOD_XEN);
    mfn_t start_mfn = maddr_to_mfn(xen_bootmodule->start), mfn;

    for_each_xen_colored_mfn ( start_mfn, mfn, i )
    {
        pte = mfn_to_xen_entry(mfn, MT_NORMAL);
        pte.pt.table = 1; /* level 3 mappings always have this bit set */
        xen_xenmap[i] = pte;
    }

    for ( i = 0; i < XEN_NR_ENTRIES(2); i++ )
    {
        vaddr_t va = BOOT_RELOC_VIRT_START + (i << XEN_PT_LEVEL_SHIFT(2));

        pte = mfn_to_xen_entry(virt_to_mfn(xen_xenmap +
                                           i * XEN_PT_LPAE_ENTRIES),
                               MT_NORMAL);
        pte.pt.table = 1;
        write_pte(&boot_second[second_table_offset(va)], pte);
    }
}

/*
 * Boot-time pagetable setup.
 * Changes here may need matching changes in head.S
 */
void __init setup_pagetables(void)
{
    uint64_t ttbr;
    lpae_t pte, *p;
    int i;

    /*
     * In case of cache coloring, map the new physical space in the boot page
     * tables. From now on, pte_of_xenaddr() will translate addresses to this
     * new space.
     */
    if ( llc_coloring_enabled )
        create_llc_coloring_mappings();

    arch_setup_page_tables();

#ifdef CONFIG_ARM_64
    pte = pte_of_xenaddr((uintptr_t)xen_first);
    pte.pt.table = 1;
    pte.pt.xn = 0;
    xen_pgtable[zeroeth_table_offset(XEN_VIRT_START)] = pte;

    p = (void *) xen_first;
#else
    p = (void *) cpu0_pgtable;
#endif

    /* Map xen second level page-table */
    p[0] = pte_of_xenaddr((uintptr_t)(xen_second));
    p[0].pt.table = 1;
    p[0].pt.xn = 0;

    /* Break up the Xen mapping into pages and protect them separately. */
    for ( i = 0; i < XEN_NR_ENTRIES(3); i++ )
    {
        vaddr_t va = XEN_VIRT_START + (i << PAGE_SHIFT);

        if ( !is_kernel(va) )
            break;
        pte = pte_of_xenaddr(va);
        pte.pt.table = 1; /* third level mappings always have this bit set */
        pte.pt.xn = 0; /* Permissions will be enforced later. Allow execution */
        xen_xenmap[i] = pte;
    }

    /* Initialise xen second level entries ... */
    /* ... Xen's text etc */
    for ( i = 0; i < XEN_NR_ENTRIES(2); i++ )
    {
        vaddr_t va = XEN_VIRT_START + (i << XEN_PT_LEVEL_SHIFT(2));

        pte = pte_of_xenaddr((vaddr_t)(xen_xenmap + i * XEN_PT_LPAE_ENTRIES));
        pte.pt.table = 1;
        xen_second[second_table_offset(va)] = pte;
    }

    /* ... Fixmap */
    pte = pte_of_xenaddr((vaddr_t)xen_fixmap);
    pte.pt.table = 1;
    xen_second[second_table_offset(FIXMAP_ADDR(0))] = pte;

#ifdef CONFIG_ARM_32
    per_cpu(xen_pgtable, 0) = cpu0_pgtable;
#endif

    if ( llc_coloring_enabled )
    {
        ttbr = virt_to_maddr(virt_to_reloc_virt(THIS_CPU_PGTABLE));
        relocate_and_switch_ttbr(ttbr);
    }
    else
    {
        ttbr = virt_to_maddr(THIS_CPU_PGTABLE);
        switch_ttbr(ttbr);
    }

    /* Protect Xen */
    for ( i = 0; i < XEN_NR_ENTRIES(3); i++ )
    {
        vaddr_t va = XEN_VIRT_START + (i << PAGE_SHIFT);
        lpae_t *entry = xen_xenmap + i;

        if ( !is_kernel(va) )
            break;

        pte = read_atomic(entry);

        if ( is_kernel_text(va) || is_kernel_inittext(va) )
        {
            pte.pt.xn = 0;
            pte.pt.ro = 1;
        }
        else if ( is_kernel_rodata(va) )
        {
            pte.pt.ro = 1;
            pte.pt.xn = 1;
        }
        else
        {
            pte.pt.xn = 1;
            pte.pt.ro = 0;
        }

        write_pte(entry, pte);
    }

    /*
     * We modified live page-tables. Ensure the TLBs are invalidated
     * before setting enforcing the WnX permissions.
     */
    flush_xen_tlb_local();

    xen_pt_enforce_wnx();
}

void *__init arch_vmap_virt_end(void)
{
    return (void *)(VMAP_VIRT_START + VMAP_VIRT_SIZE);
}

/* Release all __init and __initdata ranges to be reused */
void free_init_memory(void)
{
    paddr_t pa = virt_to_maddr(__init_begin);
    unsigned long len = __init_end - __init_begin;
    uint32_t insn;
    unsigned int i, nr = len / sizeof(insn);
    uint32_t *p;
    int rc;

    rc = modify_xen_mappings((unsigned long)__init_begin,
                             (unsigned long)__init_end, PAGE_HYPERVISOR_RW);
    if ( rc )
        panic("Unable to map RW the init section (rc = %d)\n", rc);

    /*
     * From now on, init will not be used for execution anymore,
     * so nuke the instruction cache to remove entries related to init.
     */
    invalidate_icache_local();

#ifdef CONFIG_ARM_32
    /* udf instruction i.e (see A8.8.247 in ARM DDI 0406C.c) */
    insn = 0xe7f000f0;
#else
    insn = AARCH64_BREAK_FAULT;
#endif
    p = (uint32_t *)__init_begin;
    for ( i = 0; i < nr; i++ )
        *(p + i) = insn;

    rc = destroy_xen_mappings((unsigned long)__init_begin,
                              (unsigned long)__init_end);
    if ( rc )
        panic("Unable to remove the init section (rc = %d)\n", rc);

    if ( !using_static_heap )
    {
        init_domheap_pages(pa, pa + len);
        printk("Freed %ldkB init memory.\n",
               (long)(__init_end-__init_begin) >> 10);
    }
}

/**
 * copy_from_paddr - copy data from a physical address
 * @dst: destination virtual address
 * @paddr: source physical address
 * @len: length to copy
 */
void __init copy_from_paddr(void *dst, paddr_t paddr, unsigned long len)
{
    void *src = (void *)FIXMAP_ADDR(FIX_MISC);

    while (len) {
        unsigned long l, s;

        s = paddr & (PAGE_SIZE - 1);
        l = min(PAGE_SIZE - s, len);

        set_fixmap(FIX_MISC, maddr_to_mfn(paddr), PAGE_HYPERVISOR_WC);
        memcpy(dst, src + s, l);
        clean_dcache_va_range(dst, l);
        clear_fixmap(FIX_MISC);

        paddr += l;
        dst += l;
        len -= l;
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
