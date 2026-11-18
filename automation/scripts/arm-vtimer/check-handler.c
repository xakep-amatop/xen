/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef VTIMER_TEST_BEFORE
#include <xen/init.h>
#include <xen/keyhandler.h>
#include <xen/lib.h>
#define printf printk
#else
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t register_t;
#define unlikely(x) (x)
#endif

/* Bit positions in the model of CNTV_CTL_EL0. */
enum { ENABLE = 1U << 0, IMASK = 1U << 1, ISTATUS = 1U << 2 };

static struct {
    struct {
        struct { register_t ctl; } virt_timer;
    } arch;
} guest;
static register_t timer_ctl;
static unsigned int writes, irqs;
static unsigned int virt_timer_irqs, virt_timer_spurious_irqs;
static bool irq_level;

static void set_guest_irq(bool level)
{
    irq_level = level;
    ++irqs;
    if ( level )
        printf("VTIMER_MODEL_IRQ %d\n", (timer_ctl & ISTATUS) != 0);
}

/* Run the real handler with a fake timer register and guest IRQ line. */
#define current (&guest)
#define is_idle_vcpu(v) false
#define perfc_incr(counter) (++(counter))
#define CNTV_CTL_EL0 timer_ctl
#define READ_SYSREG(reg) (reg)
#define WRITE_SYSREG(value, reg) do { (reg) = (value); ++writes; } while (0)
#define vgic_inject_irq(d, v, n, level) set_guest_irq(level)

#include "vtimer-handler.inc"

static bool check(bool expired, bool expect_irq)
{
    bool masked;

    timer_ctl = ENABLE | (expired ? ISTATUS : 0);
    guest.arch.virt_timer.ctl = 0;
    writes = irqs = 0;
    virt_timer_irqs = virt_timer_spurious_irqs = 0;
    irq_level = false;
    vtimer_interrupt(0, NULL);
    masked = (timer_ctl & IMASK) != 0;
    printf("ISTATUS=%d masked=%d writes=%u irqs=%u level=%d timer=%u spurious=%u\n",
           expired, masked, writes, irqs, irq_level, virt_timer_irqs,
           virt_timer_spurious_irqs);
    return (timer_ctl & ENABLE) && masked == expect_irq &&
           writes == expect_irq && irqs == expect_irq && irq_level == expect_irq &&
           virt_timer_irqs == expect_irq && virt_timer_spurious_irqs == !expect_irq;
}

static bool check_early_irq(bool before)
{
    bool early_ok = check(false, before);
    unsigned int early_irqs = irqs;

    /* Let the same timer reach its deadline without rearming it. */
    timer_ctl |= ISTATUS;
    if ( (timer_ctl & (ENABLE | IMASK)) == ENABLE )
        vtimer_interrupt(0, NULL);

    printf("New guest IRQs at the next deadline in the model: %u\n",
           irqs - early_irqs);
    return early_ok && irqs - early_irqs == !before;
}

static bool check_handler(bool before)
{
    bool control_ok = check(true, true);
    bool early_ok;

    printf("VTIMER_MODEL_END control\n");
    early_ok = check_early_irq(before);
    printf("VTIMER_MODEL_END early\n");
    return control_ok && early_ok;
}

#ifdef VTIMER_TEST_BEFORE
static void cf_check run_vtimer_test(unsigned char key)
{
    static bool running;

    if ( __atomic_test_and_set(&running, __ATOMIC_ACQUIRE) )
        return;

    printk("vtimer selftest %s %s\n", VTIMER_TEST_BEFORE ? "before" : "after",
           check_handler(VTIMER_TEST_BEFORE) ? "PASS" : "FAIL");
    __atomic_clear(&running, __ATOMIC_RELEASE);
}

static int __init register_vtimer_test(void)
{
    register_keyhandler('Y', run_vtimer_test, "check virtual timer IRQ handling", false);
    return 0;
}
__initcall(register_vtimer_test);
#else
int main(int argc, char **argv)
{
    bool before = argc == 2 && strcmp(argv[1], "before") == 0;

    if ( !before && (argc != 2 || strcmp(argv[1], "after") != 0) )
        return 2;

    return !check_handler(before);
}
#endif
