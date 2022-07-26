// SPDX-License-Identifier: GPL-2.0
/*
 * xen/drivers/thermal_gov_static_level.c
 *
 * Decrease throttling to certain level in case of trip
 *
 * Copyright (c) 2022 Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 */

#include <xen/cpufreq.h>
#include <xen/init.h>
#include <xen/thermal.h>

static int delay = 250;
static long freq_level = 0;

static int static_level_throttle(int sensor_id,
        struct thermal_sensor *sensor, int trip)
{
    int trip_temp, temperature, relation;
    int ret;
    enum thermal_trend trend;
    bool throttle;
    struct cpufreq_policy *cur_policy;
    unsigned long new_freq;

    ret = sensor->ops->get_trend(sensor, trip, &trend);
    if ( ret )
    {
        printk(XENLOG_ERR "%s: unable to read trend: %d\n", __func__, ret);
        return ret;
    }

    if (trip == THERMAL_TRIPS_NONE)
    {
        printk(XENLOG_DEBUG "[Sens %d] Disable throttling\n",
                sensor->sensor_id);
        throttle = false;
    }
    else
    {
        enum thermal_trip_type trip_type;
        sensor->ops->get_trip_temp(sensor->data, trip, &trip_temp);
        sensor->ops->get_trip_type(sensor->data, trip, &trip_type);

        ret = sensor->ops->get_temp(sensor->data, &temperature);
        if ( ret )
        {
            printk(XENLOG_ERR "%s: unable to read temp: %d\n", __func__, ret);
            return ret;
        }

        throttle = get_throttle_flag(temperature, trip_temp, trend);
        printk(XENLOG_DEBUG "Trip%d[type=%d,temp=%d]:trend=%d,throttle=%d\n",
            trip, trip_type, trip_temp, trend, throttle);
    }

    cur_policy = __cpufreq_get_policy(sensor->sensor_id);

    if ( throttle )
    {
        __cpufreq_policy_set_owner(cur_policy, OWNER_THERMAL);
        new_freq = ( freq_level ) ? freq_level : cur_policy->min;
    }
    else
    {
        __cpufreq_policy_set_owner(cur_policy, OWNER_CPUFREQ);
        new_freq = cur_policy->max;
    }

    relation = get_target_relation(trend);
    printk(XENLOG_DEBUG "[Policy]min=%d,max=%d,cur=%d,next=%ld,rel=%d\n",
            cur_policy->min, cur_policy->max, cur_policy->cur,
            new_freq, relation);

    ret = __cpufreq_driver_target(cur_policy, new_freq, relation,
                OWNER_THERMAL);

    if ( throttle )
        activate_throttle(sensor, delay, trip);
    else
        deactivate_throttle(sensor);

    return 0;
}

bool_t static_level_handle_option(const char *name, const char *val)
{
    if ( !strcmp(name, "freq_level") && val )
        freq_level = simple_strtoul(val, NULL, 0);

    if ( !strcmp(name, "delay") && val )
        delay = simple_strtoul(val, NULL, 0);

    return 0;
}

struct thermal_governor thermal_gov_static_level = {
    .name        = "static_level",
    .throttle    = static_level_throttle,
    .handle_option = static_level_handle_option,
};

static int __init init_thermal_gov_static_level(void)
{
    return thermal_register_governor(&thermal_gov_static_level);
}

__initcall(init_thermal_gov_static_level);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
