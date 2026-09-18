/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ARM_GIC_BENCH_H__
#define __ASM_ARM_GIC_BENCH_H__

#include <xen/types.h>
#include <xen/perfc.h>

/* Optional diagnostic counts; disabled unless requested on the command line. */
#ifdef CONFIG_PERF_COUNTERS
extern bool opt_gic_bench_counters;

static inline bool gic_bench_enabled(void)
{
    return opt_gic_bench_counters;
}

#define gic_bench_count(name)               \
    do {                                    \
        if ( gic_bench_enabled() )           \
            perfc_incr(name);                \
    } while ( 0 )

#define gic_bench_add(name, value)           \
    do {                                    \
        if ( gic_bench_enabled() )           \
            perfc_add(name, value);          \
    } while ( 0 )
#else
static inline bool gic_bench_enabled(void)
{
    return false;
}

#define gic_bench_count(name)      do { } while ( 0 )
#define gic_bench_add(name, value) do { } while ( 0 )
#endif

#endif /* __ASM_ARM_GIC_BENCH_H__ */
