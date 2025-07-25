#ifndef __ARM_PAGE_H__
#define __ARM_PAGE_H__

#include <public/xen.h>
#include <xen/page-size.h>
#include <asm/processor.h>
#include <asm/lpae.h>
#include <asm/sysregs.h>

/* Shareability values for the LPAE entries */
#define LPAE_SH_NON_SHAREABLE 0x0
#define LPAE_SH_UNPREDICTALE  0x1
#define LPAE_SH_OUTER         0x2
#define LPAE_SH_INNER         0x3

/*
 * Attribute Indexes.
 *
 * These are valid in the AttrIndx[2:0] field of an LPAE stage 1 page
 * table entry. They are indexes into the bytes of the MAIR*
 * registers, as defined below.
 *
 */
#define MT_DEVICE_nGnRnE 0x0
#define MT_NORMAL_NC     0x1
#define MT_NORMAL_WT     0x2
#define MT_NORMAL_WB     0x3
#define MT_DEVICE_nGnRE  0x4
#define MT_NORMAL        0x7

/*
 * LPAE Memory region attributes. Indexed by the AttrIndex bits of a
 * LPAE entry; the 8-bit fields are packed little-endian into MAIR0 and MAIR1.
 *
 * See section "Device memory" B2.7.2 in ARM DDI 0487B.a for more
 * details about the meaning of *G*R*E.
 *
 *                    ai    encoding
 *   MT_DEVICE_nGnRnE 000   0000 0000  -- Strongly Ordered/Device nGnRnE
 *   MT_NORMAL_NC     001   0100 0100  -- Non-Cacheable
 *   MT_NORMAL_WT     010   1010 1010  -- Write-through
 *   MT_NORMAL_WB     011   1110 1110  -- Write-back
 *   MT_DEVICE_nGnRE  100   0000 0100  -- Device nGnRE
 *   ??               101
 *   reserved         110
 *   MT_NORMAL        111   1111 1111  -- Write-back write-allocate
 *
 * /!\ It is not possible to combine the definition in MAIRVAL and then
 * split because it would result to a 64-bit value that some assembler
 * doesn't understand.
 */
#define _MAIR0(attr, mt) (_AC(attr, ULL) << ((mt) * 8))
#define _MAIR1(attr, mt) (_AC(attr, ULL) << (((mt) * 8) - 32))

#define MAIR0VAL (_MAIR0(0x00, MT_DEVICE_nGnRnE)| \
                  _MAIR0(0x44, MT_NORMAL_NC)    | \
                  _MAIR0(0xaa, MT_NORMAL_WT)    | \
                  _MAIR0(0xee, MT_NORMAL_WB))

#define MAIR1VAL (_MAIR1(0x04, MT_DEVICE_nGnRE) | \
                  _MAIR1(0xff, MT_NORMAL))

#define MAIRVAL (MAIR1VAL << 32 | MAIR0VAL)

/*
 * Layout of the flags used for updating the hypervisor page tables
 *
 * [0:2] Memory Attribute Index
 * [3:4] Permission flags
 * [5]   Page present
 * [6]   Only populate page tables
 * [7]   Superpage mappings is allowed
 * [8]   Set contiguous bit (internal flag)
 */
#define PAGE_AI_MASK(x) ((x) & 0x7U)

#define _PAGE_XN_BIT    3
#define _PAGE_RO_BIT    4
#define _PAGE_XN    (1U << _PAGE_XN_BIT)
#define _PAGE_RO    (1U << _PAGE_RO_BIT)
#define PAGE_XN_MASK(x) (((x) >> _PAGE_XN_BIT) & 0x1U)
#define PAGE_RO_MASK(x) (((x) >> _PAGE_RO_BIT) & 0x1U)

#define _PAGE_PRESENT    (1U << 5)
#define _PAGE_POPULATE   (1U << 6)

#define _PAGE_BLOCK_BIT     7
#define _PAGE_BLOCK         (1U << _PAGE_BLOCK_BIT)

#define _PAGE_CONTIG_BIT    8
#define _PAGE_CONTIG        (1U << _PAGE_CONTIG_BIT)

/*
 * _PAGE_DEVICE and _PAGE_NORMAL are convenience defines. They are not
 * meant to be used outside of this header.
 */
#define _PAGE_DEVICE    (_PAGE_XN|_PAGE_PRESENT)
#define _PAGE_NORMAL    (MT_NORMAL|_PAGE_PRESENT)

#define PAGE_HYPERVISOR_RO      (_PAGE_NORMAL|_PAGE_RO|_PAGE_XN)
#define PAGE_HYPERVISOR_RX      (_PAGE_NORMAL|_PAGE_RO)
#define PAGE_HYPERVISOR_RW      (_PAGE_NORMAL|_PAGE_XN)

#define PAGE_HYPERVISOR         PAGE_HYPERVISOR_RW
#define PAGE_HYPERVISOR_NOCACHE (_PAGE_DEVICE|MT_DEVICE_nGnRE)
#define PAGE_HYPERVISOR_WC      (_PAGE_DEVICE|MT_NORMAL_NC)

/*
 * Stage 2 Memory Type.
 *
 * These are valid in the MemAttr[3:0] field of an LPAE stage 2 page
 * table entry.
 *
 */
#define MATTR_DEV     0x1
#define MATTR_MEM_NC  0x5
#define MATTR_MEM     0xf

/* Flags for get_page_from_gva, gvirt_to_maddr etc */
#define GV2M_READ  (0u<<0)
#define GV2M_WRITE (1u<<0)
#define GV2M_EXEC  (1u<<1)

#ifndef __ASSEMBLY__

#include <xen/errno.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <asm/atomic.h>
#include <asm/system.h>

#if defined(CONFIG_ARM_32)
# include <asm/arm32/page.h>
#elif defined(CONFIG_ARM_64)
# include <asm/arm64/page.h>
#else
# error "unknown ARM variant"
#endif

/* Architectural minimum cacheline size is 4 32-bit words. */
#define MIN_CACHELINE_BYTES 16
/* Min dcache line size on the boot CPU. */
extern size_t dcache_line_bytes;

#define copy_page(dp, sp) memcpy(dp, sp, PAGE_SIZE)

#define clear_page_hot  clear_page
#define clear_page_cold clear_page

#define scrub_page_hot(page) memset(page, SCRUB_BYTE_PATTERN, PAGE_SIZE)
#define scrub_page_cold      scrub_page_hot

static inline size_t read_dcache_line_bytes(void)
{
    register_t ctr;

    /* Read CTR */
    ctr = READ_SYSREG(CTR_EL0);

    /* Bits 16-19 are the log2 number of words in the cacheline. */
    return (size_t) (4 << ((ctr >> 16) & 0xf));
}

/* Functions for flushing medium-sized areas.
 * if 'range' is large enough we might want to use model-specific
 * full-cache flushes. */

static inline int invalidate_dcache_va_range(const void *p, unsigned long size)
{
    size_t cacheline_mask = dcache_line_bytes - 1;
    unsigned long idx = 0;

    if ( !size )
        return 0;

    /* Passing a region that wraps around is illegal */
    ASSERT(((uintptr_t)p + size - 1) >= (uintptr_t)p);

    dsb(sy);           /* So the CPU issues all writes to the range */

    if ( (uintptr_t)p & cacheline_mask )
    {
        size -= dcache_line_bytes - ((uintptr_t)p & cacheline_mask);
        p = (void *)((uintptr_t)p & ~cacheline_mask);
        asm_inline volatile (
            __clean_and_invalidate_dcache_one(0) :: "r" (p) );
        p += dcache_line_bytes;
    }

    for ( ; size >= dcache_line_bytes;
            idx += dcache_line_bytes, size -= dcache_line_bytes )
        asm volatile (__invalidate_dcache_one(0) : : "r" (p + idx));

    if ( size > 0 )
        asm_inline volatile (
            __clean_and_invalidate_dcache_one(0) :: "r" (p + idx) );

    dsb(sy);           /* So we know the flushes happen before continuing */

    return 0;
}

static inline int clean_dcache_va_range(const void *p, unsigned long size)
{
    size_t cacheline_mask = dcache_line_bytes - 1;
    unsigned long idx = 0;

    if ( !size )
        return 0;

    /* Passing a region that wraps around is illegal */
    ASSERT(((uintptr_t)p + size - 1) >= (uintptr_t)p);

    dsb(sy);           /* So the CPU issues all writes to the range */
    size += (uintptr_t)p & cacheline_mask;
    size = (size + cacheline_mask) & ~cacheline_mask;
    p = (void *)((uintptr_t)p & ~cacheline_mask);
    for ( ; size >= dcache_line_bytes;
            idx += dcache_line_bytes, size -= dcache_line_bytes )
        asm_inline volatile ( __clean_dcache_one(0) : : "r" (p + idx) );
    dsb(sy);           /* So we know the flushes happen before continuing */
    /* ARM callers assume that dcache_* functions cannot fail. */
    return 0;
}

static inline int clean_and_invalidate_dcache_va_range
    (const void *p, unsigned long size)
{
    size_t cacheline_mask = dcache_line_bytes - 1;
    unsigned long idx = 0;

    if ( !size )
        return 0;

    /* Passing a region that wraps around is illegal */
    ASSERT(((uintptr_t)p + size - 1) >= (uintptr_t)p);

    dsb(sy);         /* So the CPU issues all writes to the range */
    size += (uintptr_t)p & cacheline_mask;
    size = (size + cacheline_mask) & ~cacheline_mask;
    p = (void *)((uintptr_t)p & ~cacheline_mask);
    for ( ; size >= dcache_line_bytes;
            idx += dcache_line_bytes, size -= dcache_line_bytes )
        asm volatile (__clean_and_invalidate_dcache_one(0) : : "r" (p + idx));
    dsb(sy);         /* So we know the flushes happen before continuing */
    /* ARM callers assume that dcache_* functions cannot fail. */
    return 0;
}

/* Macros for flushing a single small item.  The predicate is always
 * compile-time constant so this will compile down to 3 instructions in
 * the common case. */
#define clean_dcache(x) do {                                            \
    typeof(x) *_p = &(x);                                               \
    if ( sizeof(x) > MIN_CACHELINE_BYTES || sizeof(x) > alignof(x) )    \
        clean_dcache_va_range(_p, sizeof(x));                           \
    else                                                                \
        asm_inline volatile (                                           \
            "dsb sy;"   /* Finish all earlier writes */                 \
            __clean_dcache_one(0)                                       \
            "dsb sy;"   /* Finish flush before continuing */            \
            : : "r" (_p), "m" (*_p));                                   \
} while (0)

#define clean_and_invalidate_dcache(x) do {                             \
    typeof(x) *_p = &(x);                                               \
    if ( sizeof(x) > MIN_CACHELINE_BYTES || sizeof(x) > alignof(x) )    \
        clean_and_invalidate_dcache_va_range(_p, sizeof(x));            \
    else                                                                \
        asm_inline volatile (                                           \
            "dsb sy;"   /* Finish all earlier writes */                 \
            __clean_and_invalidate_dcache_one(0)                        \
            "dsb sy;"   /* Finish flush before continuing */            \
            : : "r" (_p), "m" (*_p));                                   \
} while (0)

/*
 * Write a pagetable entry.
 *
 * It is the responsibility of the caller to issue an ISB (if a new entry)
 * or a TLB flush (if modified or removed) after write_pte().
 */
static inline void write_pte(lpae_t *p, lpae_t pte)
{
    /* Ensure any writes have completed with the old mappings. */
    dsb(sy);
    /* Safely write the entry. This should always be an atomic write. */
    write_atomic(p, pte);
    dsb(sy);
}


/* Flush the dcache for an entire page. */
void flush_page_to_ram(unsigned long mfn, bool sync_icache);

/* Print a walk of the hypervisor's page tables for a virtual addr. */
extern void dump_hyp_walk(vaddr_t addr);

static inline uint64_t va_to_par(vaddr_t va)
{
    uint64_t par = __va_to_par(va);
    /* It is not OK to call this with an invalid VA */
    if ( par & PAR_F )
    {
        dump_hyp_walk(va);
        panic_PAR(par);
    }
    return par;
}

static inline int gva_to_ipa(vaddr_t va, paddr_t *paddr, unsigned int flags)
{
    uint64_t par = gva_to_ipa_par(va, flags);
    if ( par & PAR_F )
        return -EFAULT;
    *paddr = (par & PADDR_MASK & PAGE_MASK) | ((unsigned long) va & ~PAGE_MASK);
    return 0;
}

/* Bits in the PAR returned by va_to_par */
#define PAR_FAULT 0x1

#endif /* __ASSEMBLY__ */

#endif /* __ARM_PAGE_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
