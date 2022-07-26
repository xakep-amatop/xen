/*
 *  i.MX8 SC firmware thermal driver.
 *
 *  Copyright 2018-2020 NXP.
 *  Based on drivers/thermal/imx_sc_thermal.c
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  Based on Linux drivers/thermal/imx_sc_thermal
 *  => commit a11753a89ec610768301d4070e10b8bd60fde8cd
 *  git://source.codeaurora.org/external/imx/linux-imx
 *  branch: lf-5.10.y
 *
 *  Xen modification:
 *  Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 *  Copyright (C) 2022 EPAM Systems Inc.
 *
 */

#include <asm/sci.h>
#include <xen/device_tree.h>
#include <xen/delay.h>
#include <xen/err.h>
#include <xen/vmap.h>
#include <xen/irq.h>
#include <xen/shutdown.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <asm/device.h>
#include <asm/io.h>
#include <xen/timer.h>
#include <xen/thermal.h>

#include "../platforms/scfw_export_hyper/svc/misc/misc_api.h"
#include "asm-arm/delay.h"
#include "xen/config.h"
#include "xen/lib.h"

#define dev_name(dev) dt_node_full_name(dev_to_dt(dev))
#define TENTH(temp) temp % 1000
#define CELSIUS(temp) (temp - TENTH(temp)) / 1000;
#define GET_TEMP(celsius, tenths) celsius * 1000 + tenths * 100

#define PASSIVE     "passive"
#define PASSIVE_ID  0
#define CRITICAL    "critical"
#define CRITICAL_ID 1

#define MAX_SENSORS 2
#define TRIP_MAX    2

struct imx_sc_trip {
    int temp;
    int hyst;
    int type;
};

struct imx_sc_sensor {
    int sensor_id;
    uint32_t resource_id;
    int last_temp;
    bool notified;
    unsigned int polling_delay;
    unsigned int polling_delay_passive;
    struct imx_sc_trip trips[TRIP_MAX];
    struct timer timer;
};

struct imx_sc_thermal_priv {
    struct dt_device_node *np;
    spinlock_t lock;
    struct imx_sc_sensor *sensors[MAX_SENSORS];
};

static struct imx_sc_thermal_priv *thermal_priv;

static int imx_sc_thermal_get_temp_internal(void *data, int *temp)
{
    int ret;
    int16_t celsius = 0;
    int8_t tenths = 0;
    struct imx_sc_sensor *sensor = data;

    ret = sc_misc_get_temp(mu_ipcHandle, sensor->resource_id, SC_MISC_TEMP,
            &celsius, &tenths);

    if ( ret )
    {
        /*
         * if the SS power domain is down, read temp will fail, so
         * we can print error once and return 0 directly.
         */
        printk(XENLOG_ERR
            "read temp sensor %d failed, could be SS powered off, ret %d\n",
            sensor->resource_id, ret);
        *temp = 0;
        return 0;
    }

    *temp = GET_TEMP(celsius, tenths);

    return 0;
}

static int imx_sc_thermal_get_temp(void *data, int *temp)
{
    int ret;
    ret = imx_sc_thermal_get_temp_internal(data, temp);

    if ( ret )
        return ret;

    return 0;
}

static int imx_sc_thermal_get_trip_temp(void *data,
        int trip, int *trip_temp)
{
    struct imx_sc_sensor *sensor = data;
    *trip_temp = sensor->trips[trip].temp;
    return 0;
}

static int imx_sc_thermal_get_trip_type(void *data,
        int trip, enum thermal_trip_type *trip_type)
{
    struct imx_sc_sensor *sensor = data;
    *trip_type = sensor->trips[trip].type;
    return 0;
}

static int imx_sc_thermal_get_trend(struct thermal_sensor *sensor, int trip,
        enum thermal_trend *trend)
{
    int trip_temp, temp;
    int celsius;
    int ret;
    struct imx_sc_sensor *sc_sensor = sensor->data;

    ret = imx_sc_thermal_get_temp_internal(sc_sensor, &temp);
    if ( ret )
        return ret;

    celsius = CELSIUS(temp);

    if ( trip == THERMAL_TRIPS_NONE )
        goto out;

    if ( celsius > sc_sensor->last_temp )
        *trend = THERMAL_TREND_RAISING;
    else if ( celsius < sc_sensor->last_temp )
        *trend = THERMAL_TREND_DROPPING;
    else
    {
        *trend = THERMAL_TREND_STABLE;
        goto skip;
    }

    ret = imx_sc_thermal_get_trip_temp(sc_sensor, trip, &trip_temp);
    if ( ret )
        return ret;

    if ( (*trend == THERMAL_TREND_DROPPING) &&
            (temp <= trip_temp) )
        *trend = THERMAL_TREND_DROP_FULL;
out:
    sc_sensor->last_temp = celsius;
skip:
    return 0;
}

#define CPU_THERMAL0 "cpu-thermal0"
#define CPU_THERMAL1 "cpu-thermal1"

static int __init get_cpu_from_dt_node(struct dt_device_node *node)
{
    if ( strcmp(node->name, CPU_THERMAL0 ) == 0)
        return 0;

    if ( strcmp(node->name, CPU_THERMAL1 ) == 0)
        return 4;

    return -ENOENT;
}

static int __init imx_dt_get_sensor_id(struct dt_device_node *node, uint32_t *id)
{
    struct dt_phandle_args sensor_specs;
    int ret;

    ret = dt_parse_phandle_with_args(node,
            "thermal-sensors",
            "#thermal-sensor-cells",
            0,
            &sensor_specs);

    if ( ret )
        return ret;

    if ( sensor_specs.args_count > 1 )
    {
        printk(XENLOG_WARNING "%s: too many cells in sensor specifier %d\n",
                node->name, sensor_specs.args_count);
    }

    *id = sensor_specs.args_count ? sensor_specs.args[0] : 0;
    return 0;
}

static int __init imx_dt_get_trips(struct dt_device_node *node,
        struct imx_sc_sensor *sensor)
{
    struct dt_device_node *child, *np;
    int ret;
    u32 temp;
    u32 hyst;
    const char *type;
    int id = -1;

    np = dt_find_node_by_name(node, "trips");
    if ( !np )
        return -ENODEV;

    dt_for_each_child_node(np, child)
    {
        ret = dt_property_read_string(child, "type", &type);
        if ( ret )
            return -ENOENT;

        ret = dt_property_read_u32(child, "temperature", &temp);
        if ( !ret )
            return -ENOENT;

        ret = dt_property_read_u32(child, "hysteresis", &hyst);
        if ( !ret )
            return -ENOENT;

        if ( strcmp(type, PASSIVE) == 0 )
        {
            id = PASSIVE_ID;
            sensor->trips[id].type = THERMAL_TRIP_PASSIVE;
        }
        else if ( strcmp(type, CRITICAL) == 0 )
        {
            id = CRITICAL_ID;
            sensor->trips[id].type = THERMAL_TRIP_CRITICAL;
        }
        else
        {
            printk(XENLOG_WARNING "Unknown trip type %s. Ignorig.\n", type);
            continue;
        }

        sensor->trips[id].temp = temp;
        sensor->trips[id].hyst = hyst;
    }

    return 0;
}

static int imx_sc_thermal_throttle(int sensor_id, void *data)
{
    struct imx_sc_sensor *sensor = data;
    int temp, ret;

    ret = imx_sc_thermal_get_temp_internal(data, &temp);
    if ( ret )
        return ret;

    if ( (sensor->trips[CRITICAL_ID].temp) &&
        (temp >= sensor->trips[CRITICAL_ID].temp) )
    {
        printk(XENLOG_WARNING
                "Reached critical temperature (%d C): rebooting machine\n",
            temp / 1000);

        machine_restart(0);
    }

    return 0;
}

bool get_notify_flag(struct imx_sc_sensor *sensor, bool notify)
{
    bool result = false;
    if ( !sensor->notified && notify )
    {
        result = true;
        sensor->notified = true;
    }
    else if ( sensor->notified && !notify )
        sensor->notified = false;

    return result;
}

static void imx_sc_thermal_work(void *data)
{
    int ret;
    int temp = 0;
    bool notify = false;
    int delay, trip = THERMAL_TRIPS_NONE;
    struct imx_sc_sensor *sensor = data;

    ret = imx_sc_thermal_get_temp(sensor, &temp);
    if ( ret )
    {
        printk(XENLOG_WARNING "Unable to read temp from sensor: %d",
                sensor->resource_id);
        return;
    }

    delay = sensor->polling_delay;

    if ( temp >= sensor->trips[PASSIVE_ID].temp +
        sensor->trips[PASSIVE_ID].hyst )
    {
        trip = PASSIVE_ID;
        notify = true;
    }
    else if ( temp >= sensor->trips[CRITICAL_ID].temp +
        sensor->trips[CRITICAL_ID].hyst )
    {
        trip = CRITICAL_ID;
        notify = true;
    }

    if ( get_notify_flag(sensor, notify) )
        thermal_notify(sensor->sensor_id, sensor, trip);

    set_timer(&sensor->timer, NOW() + MILLISECS(delay));
}

static struct thermal_sensor_ops imx_thermal_sensor_ops = {
    .get_temp = imx_sc_thermal_get_temp,
    .get_trend = imx_sc_thermal_get_trend,
    .get_trip_temp = imx_sc_thermal_get_trip_temp,
    .get_trip_type = imx_sc_thermal_get_trip_type,
    .throttle = imx_sc_thermal_throttle
};

static int __init imx_sc_thermal_probe(struct dt_device_node *np)
{
    struct dt_device_node *child;
    struct imx_sc_sensor *sensor;
    int index = 0;
    int cpu;
    int ret;

    if ( thermal_priv )
        return -EEXIST;

    thermal_priv = xzalloc(struct imx_sc_thermal_priv);
    if ( !thermal_priv )
        return -ENOMEM;

    spin_lock_init(&thermal_priv->lock);
    thermal_priv->np = np;

    np = dt_find_node_by_name(NULL, "thermal-zones");
    if ( !np )
        return -ENODEV;

    dt_for_each_child_node(np, child)
    {
        cpu = get_cpu_from_dt_node(child);
        if ( cpu < 0 )
            continue;

        if ( index >= MAX_SENSORS )
            break;

        sensor = xzalloc(struct imx_sc_sensor);
        if ( !sensor )
            goto err_free;

        ret = dt_property_read_u32(child, "polling-delay",
                &sensor->polling_delay);
        if ( !ret )
            return -ENOENT;

        ret = dt_property_read_u32(child, "polling-delay-passive",
                &sensor->polling_delay_passive);
        if ( !ret )
            return -ENOENT;

        ret = imx_dt_get_sensor_id(child, &sensor->resource_id);
        if ( ret < 0 )
        {
            printk(XENLOG_ERR
                "failed to get valid sensor resource id: %d\n",
                ret);
            break;
        }

        ret = imx_dt_get_trips(child, sensor);
        if ( ret )
        {
            printk(XENLOG_ERR "Wrong format of the trip dt node\n");
            break;
        }

        sensor->sensor_id = cpu;
        sensor->notified = false;

        ret = register_thermal_sensor(cpu, &imx_thermal_sensor_ops, sensor, 2);
        if ( ret )
        {
            printk(XENLOG_WARNING "Unable to register sensor %d\n", cpu);
            return ret;
        }
        init_timer(&sensor->timer, imx_sc_thermal_work, (void *)sensor, cpu);
        set_timer(&sensor->timer, NOW());

        thermal_priv->sensors[index++] = sensor;
    }

    return 0;

err_free:
    xfree(thermal_priv);

    return ret;
}

static int __init imx_sc_thermal_driver_init(void)
{
    int ret;
    struct dt_device_node *np;

    np = dt_find_compatible_node(NULL, NULL, "fsl,imx-sc-thermal");
    if ( !np )
    {
        printk(XENLOG_WARNING "Can't find thermal node\n");
        return -ENODEV;
    }

    dt_device_set_used_by(np, DOMID_XEN);

    ret = imx_sc_thermal_probe(np);
    if ( ret )
    {
        printk(XENLOG_ERR "%s: failed to init i.MX8 SC THS (%d)\n",
                dev_name(&np->dev), ret);
        return ret;
    }

    return 0;
}
__initcall(imx_sc_thermal_driver_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
