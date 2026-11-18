/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sw_isr_table.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/arch/arm64/timer.h>

static struct _isr_table_entry original_isr;
static atomic_t irq_count;
static atomic_t spurious_count;
static atomic_t expiry_count;

/* Observe the IRQ, then invoke the unchanged Zephyr timer handler. */
static void observe_timer_irq(const void *unused)
{
	uint64_t ctl = read_cntv_ctl_el0();

	ARG_UNUSED(unused);
	atomic_inc(&irq_count);
	if ((ctl & (1U << 2)) == 0U) {
		atomic_inc(&spurious_count);
	}
	original_isr.isr(original_isr.arg);
}

static void expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	atomic_inc(&expiry_count);
}

int main(void)
{
	struct k_timer timer;
	uint64_t frequency = read_cntfrq_el0();
	uint64_t start, elapsed;
	unsigned int key;
	int irq;

	key = irq_lock();
	original_isr = _sw_isr_table[ARM_ARCH_TIMER_IRQ - CONFIG_GEN_IRQ_START_VECTOR];
	irq_disable(ARM_ARCH_TIMER_IRQ);
	irq = irq_connect_dynamic(ARM_ARCH_TIMER_IRQ, ARM_ARCH_TIMER_PRIO,
				  observe_timer_irq, NULL, ARM_ARCH_TIMER_FLAGS);
	/* GICv3 priority configuration disables the interrupt. */
	irq_enable(ARM_ARCH_TIMER_IRQ);
	irq_unlock(key);
	if (irq != ARM_ARCH_TIMER_IRQ) {
		printk("VTIMER_PROBE_FAIL connect=%d\n", irq);
		return 1;
	}

	k_timer_init(&timer, expired, NULL);
	start = read_cntvct_el0();
	k_timer_start(&timer, K_MSEC(1500), K_NO_WAIT);
	printk("VTIMER_READY ctl=%lx tickless=%d irq=%d\n",
	       (unsigned long)read_cntv_ctl_el0(),
	       IS_ENABLED(CONFIG_TICKLESS_KERNEL), ARM_ARCH_TIMER_IRQ);
	while (atomic_get(&expiry_count) == 0) {
		/* The hardware counter still advances if kernel time stops. */
		if ((read_cntvct_el0() - start) > frequency * 5U) {
			printk("VTIMER_PROBE_FAIL timeout ctl=%lx irqs=%ld\n",
			       (unsigned long)read_cntv_ctl_el0(),
			       (long)atomic_get(&irq_count));
			return 1;
		}
	}
	elapsed = read_cntvct_el0() - start;
	if (elapsed < frequency * 14U / 10U) {
		printk("VTIMER_PROBE_FAIL early expiry\n");
		return 1;
	}
	printk("VTIMER_FIRST_EXPIRY elapsed_us=%llu\n",
	       (unsigned long long)(elapsed * 1000000U / frequency));

	atomic_set(&expiry_count, 0);
	k_timer_start(&timer, K_MSEC(10), K_MSEC(10));
	for (unsigned int i = 0; i < 30U; i++) {
		k_msleep(10);
	}
	k_timer_stop(&timer);
	if (atomic_get(&expiry_count) < 20) {
		printk("VTIMER_PROBE_FAIL missing periodic expiry\n");
		return 1;
	}
	printk("VTIMER_PROBE_PASS expiries=%ld irqs=%ld spurious=%ld\n",
	       (long)atomic_get(&expiry_count), (long)atomic_get(&irq_count),
	       (long)atomic_get(&spurious_count));
	return 0;
}
