/******************************************************************************
 * watchdog.h
 *
 * Common watchdog code
 */

#ifndef __XEN_WATCHDOG_H__
#define __XEN_WATCHDOG_H__

#include <xen/types.h>
#include <xen/timer.h>

struct watchdog_timer {
    struct timer timer;
    uint32_t timeout;
};

#ifdef CONFIG_WATCHDOG

/* Try to set up a watchdog. */
void watchdog_setup(void);

/* Enable the watchdog. */
void watchdog_enable(void);

/* Disable the watchdog. */
void watchdog_disable(void);

/* Is the watchdog currently enabled. */
bool watchdog_enabled(void);

#else

#define watchdog_setup() ((void)0)
#define watchdog_enable() ((void)0)
#define watchdog_disable() ((void)0)
#define watchdog_enabled() ((void)0)

#endif

#endif /* __XEN_WATCHDOG_H__ */
