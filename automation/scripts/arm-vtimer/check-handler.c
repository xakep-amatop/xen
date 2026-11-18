/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t register_t;

/* Bit positions in the model of CNTV_CTL_EL0. */
enum { ENABLE = 1U << 0, IMASK = 1U << 1, ISTATUS = 1U << 2 };

static struct {
    struct {
        struct { register_t ctl; } virt_timer;
    } arch;
} guest;
static register_t timer_ctl;
static unsigned int writes, irqs;
static bool irq_level;

/* Run the real handler with a fake timer register and guest IRQ line. */
#define current (&guest)
#define is_idle_vcpu(v) false
#define unlikely(x) (x)
#define perfc_incr(counter) ((void)0)
#define CNTV_CTL_EL0 timer_ctl
#define READ_SYSREG(reg) (reg)
#define WRITE_SYSREG(value, reg) do { (reg) = (value); ++writes; } while (0)
#define vgic_inject_irq(d, v, n, level) do { irq_level = (level); ++irqs; } while (0)

#include "handler.inc"

static bool check(bool expired, bool expect_irq)
{
    bool masked;

    timer_ctl = ENABLE | (expired ? ISTATUS : 0);
    guest.arch.virt_timer.ctl = 0;
    writes = irqs = 0;
    irq_level = false;
    vtimer_interrupt(0, NULL);
    masked = (timer_ctl & IMASK) != 0;
    printf("ISTATUS=%d masked=%d writes=%u irqs=%u level=%d\n",
           expired, masked, writes, irqs, irq_level);
    return (timer_ctl & ENABLE) && masked == expect_irq &&
           writes == expect_irq && irqs == expect_irq && irq_level == expect_irq;
}

int main(int argc, char **argv)
{
    bool before = argc == 2 && strcmp(argv[1], "before") == 0;

    if ( !before && (argc != 2 || strcmp(argv[1], "after") != 0) )
        return 2;

    return !(check(true, true) && check(false, before));
}
