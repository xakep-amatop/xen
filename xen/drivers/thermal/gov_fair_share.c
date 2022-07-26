// SPDX-License-Identifier: GPL-2.0-only
/*
 *  fair_share.c - A simple weight based Thermal governor
 *
 *  Copyright (C) 2012 Intel Corp
 *  Copyright (C) 2012 Durgadoss R <durgadoss.r@intel.com>
 *
 *  Ported to Xen:
 *  xen/drivers/thermal/gov_fair_share.c
 *
 *  Copyright (C) 2022 Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include "xen/compiler.h"
#include <xen/cpufreq.h>
#include <xen/init.h>
#include <xen/thermal.h>

static int usr_delay = 250;
#define LAST_TRIP_EXTENT  50;

/**
 * get_trip_level: - obtains the current trip level for a zone
 * @sensor: pointer to sensor structure
 */
static int get_trip_level(struct thermal_sensor *sensor)
{
    int count = 0;
    int trip_temp;
    int temperature;

    if ( unlikely(sensor->trips == 0 || !sensor->ops->get_trip_temp) )
        return 0;

    sensor->ops->get_temp(sensor->data, &temperature);

    for (count = 0; count < sensor->trips; count++)
    {
        sensor->ops->get_trip_temp(sensor->data, count, &trip_temp);
        if ( temperature < trip_temp )
            break;
    }

    return count;
}

static long get_target_freq(struct cpufreq_policy *policy, int temp,
        int trip_temp, int next_trip_temp)
{
    int percentage;
    int diff_freq;

    if ( temp < trip_temp || trip_temp == next_trip_temp )
        return policy->max;

    if ( trip_temp >= next_trip_temp )
        return policy->min;

    percentage = ((temp - trip_temp) * 100) / (next_trip_temp - trip_temp) ;
    diff_freq = ((policy->max - policy->min) * percentage) / 100;

    return policy->max - diff_freq;
}

/**
 * fair_share_throttle - throttles devices associated with the given zone
 * @sensor_id: id of the sensor
 * @sensor: pointer to sensor structure
 * @trip: trip point index
 *
 * Throttling Logic: Set cpu cluster frquency based on the temperature level.
 *
 * Parameters used for Throttling:
 * P1. trip_temp, next_trip_temp - temperature limits in this trip
 * P2. percentage/100:
 *    How 'effective' the device is based on temperature and trips.
 *    Calculating based on P1.
 * P3. policy frequency limits:
 *    The descrition of the minimal and maximum frquency, that can be set for
 *    current cluster.
 *    new_freq = policy.max - (policy.max - policy.min) * P2.
 */
static int fair_share_throttle(int sensor_id, struct thermal_sensor *sensor,
        int trip)
{
    enum thermal_trend trend;
    int trip_level = get_trip_level(sensor);
    struct cpufreq_policy *cur_policy;
    int next_trip_temp, trip_temp;
    int temp;
    bool throttle = false;
    int target_freq;
    int relation;
    int ret;

    sensor->ops->get_temp(sensor->data, &temp);
    sensor->ops->get_trip_temp(sensor->data, trip, &trip_temp);

    if ( trip_level != sensor->trips )
        sensor->ops->get_trip_temp(sensor->data, trip_level, &next_trip_temp);
    else
        next_trip_temp = trip_temp + LAST_TRIP_EXTENT;

    sensor->ops->get_trend(sensor, trip, &trend);

    cur_policy = __cpufreq_get_policy(sensor->sensor_id);

    if ( trip == THERMAL_TRIPS_NONE )
    {
        throttle = false;
        target_freq = cur_policy->max;
    }
    else
    {
        throttle = get_throttle_flag(temp, trip_temp, trend);
        target_freq = get_target_freq(cur_policy, temp, trip_temp,
                next_trip_temp);
    }

    if ( throttle )
        __cpufreq_policy_set_owner(cur_policy, OWNER_THERMAL);
    else
        __cpufreq_policy_set_owner(cur_policy, OWNER_CPUFREQ);

    relation = get_target_relation(trend);
    printk(XENLOG_DEBUG
        "Trip%d[temp=%d,next_temp=%d,ctemp=%d]:trend=%d,throttle=%d,freq=%d\n",
        trip, trip_temp, next_trip_temp, temp, trend, throttle, target_freq);

    ret = __cpufreq_driver_target(cur_policy, target_freq, relation,
                OWNER_THERMAL);

    if ( throttle )
        activate_throttle(sensor, usr_delay, trip);
    else
        deactivate_throttle(sensor);

    return ret;
}

bool_t fair_share_handle_option(const char *name, const char *val)
{
    if ( !strcmp(name, "delay") && val )
        usr_delay = simple_strtoul(val, NULL, 0);

    return 0;
}

struct thermal_governor thermal_gov_fair_share = {
    .name        = "fair_share",
    .throttle    = fair_share_throttle,
    .handle_option = fair_share_handle_option,
};

static int __init init_thermal_gov_fair_share(void)
{
    return thermal_register_governor(&thermal_gov_fair_share);
}
__initcall(init_thermal_gov_fair_share);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
