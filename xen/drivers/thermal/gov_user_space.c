// SPDX-License-Identifier: GPL-2.0-only
/*
 *  user_space.c - A simple user space Thermal events notifier
 *
 *  Copyright (C) 2012 Intel Corp
 *  Copyright (C) 2012 Durgadoss R <durgadoss.r@intel.com>
 *
 *  Ported to Xen:
 *  xen/drivers/thermal/gov_user_space.c
 *
 *  Copyright (C) 2022 Oleksii Moisieiev <oleksii_mosieiev@epam.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <xen/init.h>
#include <xen/thermal.h>

static int notify_user_space(int sensor_id, struct thermal_sensor *sensor,
        int trip)
{
    printk(XENLOG_INFO "Thermal[%d] notify trip = %d\n", sensor->sensor_id,
            trip);
    return 0;
}

struct thermal_governor thermal_gov_user_space = {
    .name        = "user_space",
    .throttle    = notify_user_space,
};

static int __init init_thermal_gov_user_space(void)
{
    return thermal_register_governor(&thermal_gov_user_space);
}
__initcall(init_thermal_gov_user_space);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
