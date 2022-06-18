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

#include "../platforms/scfw_export_hyper/svc/misc/misc_api.h"
#include "asm-arm/delay.h"
#include "xen/config.h"
#include "xen/lib.h"

extern int imx_cpufreq_throttle(bool enable, int cpu);

#define dev_name(dev) dt_node_full_name(dev_to_dt(dev))
#define CELSIUS(temp) temp >> 3
#define TENTH(temp) (temp - (temp >> 3) * 1000) / 100
#define GET_TEMP(celsius, tenths) celsius * 1000 + tenths * 100

#define PASSIVE "passive"
#define CRITICAL "critical"

#define MAX_SENSORS 16

struct imx_sc_temp {
	int temp;
	int hyst;
};

struct imx_sc_sensor {
	uint32_t resource_id;
	int cluster_cpu;
	bool throttle_enabled;
	unsigned int polling_delay;
	unsigned int polling_delay_passive;
	struct imx_sc_temp temp_passive;
	struct imx_sc_temp temp_critical;
	struct timer timer;
};

struct imx_sc_thermal_priv {
	struct dt_device_node *np;
	spinlock_t lock;
	struct imx_sc_sensor *sensors[MAX_SENSORS];
};

static struct imx_sc_thermal_priv *thermal_priv;

static int imx_sc_thermal_get_temp(void *data, int *temp)
{
	int ret;
	int16_t celsius = 0;
	int8_t tenths = 0;
	struct imx_sc_sensor *sensor = data;

	ret = sc_misc_get_temp(mu_ipcHandle, sensor->resource_id, SC_MISC_TEMP,
			&celsius, &tenths);

	if (ret) {
		/*
		 * if the SS power domain is down, read temp will fail, so
		 * we can print error once and return 0 directly.
		 */
		printk(XENLOG_ERR "read temp sensor %d failed, could be SS powered off, ret %d\n",
			     sensor->resource_id, ret);
		*temp = 0;
		return 0;
	}

	*temp = GET_TEMP(celsius, tenths);

	return 0;
}

#define CPU_THERMAL0 "cpu-thermal0"
#define CPU_THERMAL1 "cpu-thermal1"

static int __init get_cpu_from_dt_node(struct dt_device_node *node)
{
	if (strcmp(node->name, CPU_THERMAL0) == 0)
		return 0;

	if (strcmp(node->name, CPU_THERMAL1) == 0)
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

	if (sensor_specs.args_count > 1) {
		printk(XENLOG_WARNING "%s: too many cells in sensor specifier %d\n",
				node->name, sensor_specs.args_count);
	}

	*id = sensor_specs.args_count ? sensor_specs.args[0] : 0;
	return 0;
}

static int __init imx_dt_get_trips(struct dt_device_node *node,
		struct imx_sc_temp *crit, struct imx_sc_temp *passive)
{
	struct dt_device_node *child, *np;
	int ret;
	u32 temp;
	u32 hyst;
	const char *type;

	np = dt_find_node_by_name(node, "trips");
	if (!np)
		return -ENODEV;

	dt_for_each_child_node(np, child) {
		ret = dt_property_read_string(child, "type", &type);
		if (ret)
			return -ENOENT;

		ret = dt_property_read_u32(child, "temperature", &temp);
		if (!ret)
			return -ENOENT;

		ret = dt_property_read_u32(child, "hysteresis", &hyst);
		if (!ret)
			return -ENOENT;

		if (strcmp(type, PASSIVE) == 0)
		{
			passive->temp = temp;
			passive->hyst = hyst;
		}
		else if (strcmp(type, CRITICAL) == 0)
		{
			crit->temp = temp;
			crit->hyst = hyst;
		}
		else
			printk(XENLOG_WARNING "Unknown trip type %s. Ignorig.\n", type);
	}
	return 0;
}

static unsigned long do_throttling(struct imx_sc_sensor *sensor, int temp)
{
	unsigned long delay = sensor->polling_delay;

	if ((sensor->temp_critical.temp) &&
		(temp >= sensor->temp_critical.temp))
	{
		printk(XENLOG_WARNING "Reached critical temperature (%d C): rebooting machine\n",
			temp / 1000);

		machine_restart(0);
	}
	else
	{
		if (!sensor->temp_passive.temp)
			goto out;

		if (temp > sensor->temp_passive.temp)
		{
			delay = sensor->polling_delay_passive;
			if (sensor->throttle_enabled)
				goto out;

			if (imx_cpufreq_throttle(true, sensor->cluster_cpu)) {
				printk("Failed to enable CPU throttling\n");
				goto out;
			}
			sensor->throttle_enabled = true;
		}
		else if (temp < sensor->temp_passive.temp -
				sensor->temp_passive.hyst)
		{
			if (!sensor->throttle_enabled)
				goto out;

			imx_cpufreq_throttle(false, sensor->resource_id);
			sensor->throttle_enabled = false;
		}
	}

out:
	return delay;
}

static void imx_sc_thermal_work(void *data)
{
	int ret;
	unsigned long delay;
	int temp = 0;
	struct imx_sc_sensor *sensor = data;

	ret = imx_sc_thermal_get_temp(sensor, &temp);
	if (ret)
	{
		printk(XENLOG_WARNING "Unable to read temp from sensor: %d",
				sensor->resource_id);
		return;
	}

	delay = do_throttling(sensor, temp);

	set_timer(&sensor->timer, NOW() + MILLISECS(delay));
}

static int __init imx_sc_thermal_probe(struct dt_device_node *np)
{
	struct dt_device_node *child;
	struct imx_sc_sensor *sensor;
	int index = 0;
	int cpu;
	int ret;

	if (thermal_priv)
		return -EEXIST;

	thermal_priv = xzalloc(struct imx_sc_thermal_priv);
	if (!thermal_priv)
		return -ENOMEM;

	spin_lock_init(&thermal_priv->lock);
	thermal_priv->np = np;

	np = dt_find_node_by_name(NULL, "thermal-zones");
	if (!np)
		return -ENODEV;

	dt_for_each_child_node(np, child) {
		cpu = get_cpu_from_dt_node(child);
		if ( cpu < 0 )
			continue;

		if (index >= MAX_SENSORS)
			break;

		sensor = xzalloc(struct imx_sc_sensor);
		if (!sensor) {
			goto err_free;
		}

		ret = dt_property_read_u32(child, "polling-delay", &sensor->polling_delay);
		if (!ret)
			return -ENOENT;

		ret = dt_property_read_u32(child, "polling-delay-passive", &sensor->polling_delay_passive);
		if (!ret)
			return -ENOENT;

		ret = imx_dt_get_sensor_id(child, &sensor->resource_id);
		if (ret < 0) {
			printk(XENLOG_ERR
				"failed to get valid sensor resource id: %d\n",
				ret);
			break;
		}

		ret = imx_dt_get_trips(child, &sensor->temp_critical,
				&sensor->temp_passive);
		if (ret) {
			printk(XENLOG_ERR "Wrong format of the trip dt node\n");
			break;
		}

		sensor->cluster_cpu = cpu;
		sensor->throttle_enabled = false;
		init_timer(&sensor->timer, imx_sc_thermal_work, (void *)sensor, cpu);
		set_timer(&sensor->timer, NOW());

		thermal_priv->sensors[index++] = sensor;
	}

	return 0;

err_free:
	xfree(thermal_priv);

	return ret;
}

static const struct dt_device_match imx_sc_thermal_table[] __initconst = {
	{ .compatible = "fsl,imx-sc-thermal", },
	{ },
};

static int __init imx_sc_thermal_init(struct dt_device_node *np,
		const void *data)
{
	int ret;

	//We do not set used_by to DOMID_XEN because we need this node
	//to also be available for Dom0

	ret = imx_sc_thermal_probe(np);
	if (ret) {
		printk(XENLOG_ERR "%s: failed to init i.MX8 SC THS (%d)\n",
				dev_name(&np->dev), ret);
		return ret;
	}

	return 0;
}

DT_DEVICE_START(imx_sc_thermal, "i.MX8 SC THS", DEVICE_THS)
	.dt_match = imx_sc_thermal_table,
	.init = imx_sc_thermal_init,
DT_DEVICE_END
