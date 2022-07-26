/*
 *  Debug driver for thermal subsystem
 *  Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 *  Copyright (C) 2022 EPAM Systems Inc.
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
 */

#include <xen/err.h>
#include <xen/irq.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <asm/device.h>
#include <asm/io.h>
#include <xen/param.h>
#include <xen/thermal.h>

#define PASSIVE     "passive"
#define PASSIVE_ID  0
#define CRITICAL    "critical"
#define CRITICAL_ID 1

#define MAX_SENSORS 2
#define TRIP_MAX    2

#define SENSOR0_ID  0
#define SENSOR1_ID  4

struct debug_drv_sensor {
    int sensor_id;
};

struct debug_drv_thermal_priv {
    struct debug_drv_sensor *sensors[MAX_SENSORS];
};

static int __read_mostly thermal_debug_sensor0_temp = 30;
integer_runtime_param("sensor0_temp", thermal_debug_sensor0_temp);
static int __read_mostly thermal_debug_sensor1_temp = 30;
integer_runtime_param("sensor1_temp", thermal_debug_sensor1_temp);
static int __read_mostly thermal_debug_sensor0_trend = 0;
integer_runtime_param("sensor0_trend", thermal_debug_sensor0_trend);
static int __read_mostly thermal_debug_sensor1_trend = 0;
integer_runtime_param("sensor1_trend", thermal_debug_sensor1_trend);
static int __read_mostly thermal_debug_sensor0_pass_temp = 50;
integer_runtime_param("sensor0_pass_temp", thermal_debug_sensor0_pass_temp);
static int __read_mostly thermal_debug_sensor1_pass_temp = 50;
integer_runtime_param("sensor1_pass_temp", thermal_debug_sensor1_pass_temp);
static int __read_mostly thermal_debug_sensor0_crit_temp = 60;
integer_runtime_param("sensor0_crit_temp", thermal_debug_sensor0_crit_temp);
static int __read_mostly thermal_debug_sensor1_crit_temp = 60;
integer_runtime_param("sensor1_crit_temp", thermal_debug_sensor1_crit_temp);

static struct debug_drv_thermal_priv *thermal_priv;

static int debug_drv_thermal_get_temp(void *data, int *temp)
{
    struct debug_drv_sensor *sensor = data;
    *temp = ( sensor->sensor_id == SENSOR0_ID ) ? thermal_debug_sensor0_temp :
        thermal_debug_sensor1_temp;
    return 0;
}

static int debug_drv_thermal_get_trip_temp(void *data,
        int trip, int *trip_temp)
{
    struct debug_drv_sensor *sensor = data;
    if ( sensor->sensor_id == SENSOR0_ID )
    {
        *trip_temp = (trip == PASSIVE_ID) ? thermal_debug_sensor0_pass_temp :
            thermal_debug_sensor0_crit_temp;
    }
    else
    {
        *trip_temp = (trip == PASSIVE_ID) ? thermal_debug_sensor1_pass_temp :
            thermal_debug_sensor1_crit_temp;
    }

    return 0;
}

static int debug_drv_thermal_get_trip_type(void *data,
        int trip, enum thermal_trip_type *trip_type)
{
    *trip_type = (trip == PASSIVE_ID) ? THERMAL_TRIP_PASSIVE :
        THERMAL_TRIP_CRITICAL;
    return 0;
}

static int debug_drv_thermal_get_trend(struct thermal_sensor *sensor, int trip,
        enum thermal_trend *trend)
{
    *trend = (sensor->sensor_id == SENSOR0_ID) ? thermal_debug_sensor0_trend :
        thermal_debug_sensor1_trend;
    return 0;
}

static int __init get_cpu_from_id(int id)
{
    return (id == 0) ? 0 : 4;
}

static int debug_drv_thermal_throttle(int sensor_id, void *data)
{
    printk(XENLOG_INFO "Throttle [%d]\n", sensor_id);
    return 0;
}

static char notify_val[3];
static void __init notify_init(struct param_hypfs *par)
{
    memcpy(notify_val, "0:0", 3);
    custom_runtime_set_var(par, notify_val);
}

static int parse_notify(const char *s)
{
    int sensor_id = s[0] - '0';
    int trip = s[2] - '0';
    int i;

    for (i = 0; i < MAX_SENSORS; i++)
    {
        if ( thermal_priv->sensors[i]->sensor_id == sensor_id )
        {
            thermal_notify(sensor_id, thermal_priv->sensors[i],
                    (trip == 9) ? THERMAL_TRIPS_NONE : trip);
            break;
        }
    }

    return 0;
}

custom_runtime_param("thermal_notify", parse_notify, notify_init);

static struct thermal_sensor_ops debug_thermal_sensor_ops = {
    .get_temp = debug_drv_thermal_get_temp,
    .get_trend = debug_drv_thermal_get_trend,
    .get_trip_temp = debug_drv_thermal_get_trip_temp,
    .get_trip_type = debug_drv_thermal_get_trip_type,
    .throttle = debug_drv_thermal_throttle
};

static int __init debug_drv_thermal_probe(void)
{
    struct debug_drv_sensor *sensor;
    int index = 0;
    int cpu, ret, i;

    if ( thermal_priv )
        return -EEXIST;

    thermal_priv = xzalloc(struct debug_drv_thermal_priv);
    if ( !thermal_priv )
        return -ENOMEM;

    for (i = 0; i < MAX_SENSORS; i++)
    {
        cpu = get_cpu_from_id(i);
        if ( cpu < 0 )
            continue;

        sensor = xzalloc(struct debug_drv_sensor);
        if ( !sensor )
            goto err_free;

        sensor->sensor_id = cpu;

        ret = register_thermal_sensor(cpu, &debug_thermal_sensor_ops,
                                      sensor, 2);
        if ( ret )
        {
            printk(XENLOG_WARNING "Unable to register sensor %d\n", cpu);
            return ret;
        }
        thermal_priv->sensors[index++] = sensor;
    }

    return 0;

err_free:
    xfree(thermal_priv);

    return ret;
}


static int __init debug_drv_thermal_driver_init(void)
{
    int ret;

    ret = debug_drv_thermal_probe();
    if (ret) {
        printk(XENLOG_ERR "Failed to init Debug thermal driver (%d)\n", ret);
        return ret;
    }
    return 0;
}
__initcall(debug_drv_thermal_driver_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
